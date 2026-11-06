pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import "shell"

// The top bar: project and its plain-language state, the Arrange/Mixer view
// switch, the transport and time readouts, master output and project actions.
Rectangle {
    id: toolbar
    required property var bridge
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    // Plain status under the project name ("Saved · changes apply live").
    property string statusText: ""
    property bool statusWarning: false
    // "arrange", "mixer" or "mixcheck".
    property string currentView: "arrange"
    property string monoFamily: Theme.monoFamily
    property color canvasColor: Theme.canvas
    // Record's red (ADR-0086).
    property color recordColor: Theme.recordColor
    readonly property var recording: bridge.recording
    // Layout of the bar, left to right.
    readonly property int identityWidth: 232
    readonly property int identityTextX: 32
    readonly property int viewSwitchHeight: 38
    readonly property int viewButtonPadding: 28
    readonly property int transportGap: 24
    readonly property int transportSpacing: 10
    readonly property int transportButtonWidth: 40
    readonly property int transportButtonHeight: 36
    readonly property int playButtonWidth: 52
    readonly property int transportGlyphSize: 12
    readonly property int playGlyphSize: 14
    readonly property int toggleHeight: 30
    readonly property real toggleRadius: 7
    readonly property int togglePadding: 18
    readonly property int keysTogglePadding: 30
    readonly property int keysLabelX: 26
    readonly property int keysIconWidth: 14
    readonly property int keysIconHeight: 10
    readonly property int readoutsHeight: 44
    readonly property int readoutPadding: 10
    readonly property int positionReadoutWidth: 92
    readonly property int keyReadoutWidth: 82
    readonly property int readoutDividerHeight: 30
    readonly property int actionsSpacing: 14
    readonly property int meterWidth: 120
    readonly property int meterHeight: 9
    readonly property int meterBarHeight: 3
    readonly property int meterBarPitch: 4
    readonly property int meterPeakWidth: 2
    // Meter levels that turn the bar amber, then red.
    readonly property real meterWarmLevel: 0.7
    readonly property real meterHotLevel: 0.9
    readonly property int actionButtonHeight: 32
    readonly property int actionButtonPadding: 24
    readonly property int moreButtonWidth: 62
    readonly property int menuWidth: 232
    readonly property int menuRowHeight: 34
    readonly property int keyPickerWidth: 236
    readonly property int keyPickerRowHeight: 28
    readonly property int keyPickerColumns: 4

    signal playbackRequested()
    signal stopRequested()
    signal mediaRequested()
    signal revisionsRequested()
    signal audioMenuRequested()
    signal viewRequested(string view)
    signal exportRequested()
    signal mixCheckRequested()
    signal detailsRequested()
    signal inspectorRequested()
    signal masterEffectsRequested()
    signal pluginsRequested()
    signal cacheRequested()
    signal shortcutsRequested()

    property bool keyPickerOpen: false
    // Everything beyond the transport and Export lives in the More menu.
    property bool compactMenuOpen: false
    onVisibleChanged: {
        if (!visible)
            compactMenuOpen = false
    }
    readonly property var menuItems: [
        { key: "inspector", label: "Inspector", name: "editNavButton" },
        { key: "masterEffects", label: "Master effects", name: "fxNavButton" },
        { key: "plugins", label: "VST3 plug-ins", name: "openVst3Browser" },
        { key: "media", label: "Media", name: "managedMediaButton" },
        { key: "revisions", label: "Versions", name: "namedRevisionButton" },
        { key: "audio", label: "Audio · " + bridge.audioOutput.selectedAudioDeviceName, name: "audioOutputMenuButton" },
        { key: "cache", label: "Processing cache", name: "openCachePanel" },
        { key: "shortcuts", label: "Keyboard shortcuts", name: "shortcutsHelpButton" },
        { key: "details", label: "Engine details", name: "developerDetailsButton" },
        { key: "close", label: "Close project", name: "closeProjectButton" }
    ]
    function activateMenuItem(key) {
        compactMenuOpen = false
        if (key === "inspector") inspectorRequested()
        else if (key === "masterEffects") masterEffectsRequested()
        else if (key === "plugins") pluginsRequested()
        else if (key === "media") mediaRequested()
        else if (key === "revisions") revisionsRequested()
        else if (key === "audio") audioMenuRequested()
        else if (key === "cache") cacheRequested()
        else if (key === "shortcuts") shortcutsRequested()
        else if (key === "close") bridge.close_project()
        else detailsRequested()
    }

    readonly property bool playing: bridge.playbackActive && !bridge.playbackPaused
    readonly property int beatsPerBar: {
        const parts = /^(\d+)\//.exec(String(bridge.projectTiming.projectMeter || ""))
        return parts && Number(parts[1]) > 0 ? Number(parts[1]) : 4
    }
    readonly property string positionText: {
        const bar = Math.max(0, Number(bridge.playheadBar) || 0)
        const whole = Math.floor(bar)
        const beats = (bar - whole) * beatsPerBar
        const beat = Math.floor(beats)
        const sixteenth = Math.floor((beats - beat) * 4)
        // Bars before bar 1 count up to 0.
        return (whole + 1 - bridge.leadBars) + " . " + (beat + 1) + " . " + (sixteenth + 1)
    }
    function truePeakDb(value) {
        const peak = Number(value)
        return peak > 0 && isFinite(peak) ? (20 * Math.log(peak) / Math.LN10).toFixed(1) : "−∞"
    }
    readonly property string masterText: {
        const peak = bridge.masterTruePeak
        const loudness = bridge.masterSection ? bridge.masterSection.reading : undefined
        const parts = []
        if (loudness !== undefined && loudness.valid && isFinite(loudness.shortTerm))
            parts.push(Number(loudness.shortTerm).toFixed(1) + " LUFS")
        if (peak.incomplete)
            parts.push("peak unavailable")
        else if (peak.valid)
            parts.push(truePeakDb(Math.max(Number(peak.left), Number(peak.right))) + " dBTP")
        return parts.length > 0 ? parts.join(" · ") : "—"
    }

    height: Theme.toolbarHeight
    color: panelColor
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.hairline
        color: toolbar.lineColor
    }

    // Project identity and plain state.
    Item {
        id: identity
        x: Theme.spaceXl
        width: toolbar.identityWidth
        height: parent.height
        Canvas {
            id: logo
            x: 0
            // A diamond outline round a dot.
            readonly property real inset: 2
            readonly property real dotRadius: 3
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.logoSize
            height: Theme.logoSize
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                context.strokeStyle = toolbar.accentColor
                context.lineWidth = Theme.strokeBold
                context.beginPath()
                context.moveTo(width / 2, inset)
                context.lineTo(width - inset, height / 2)
                context.lineTo(width / 2, height - inset)
                context.lineTo(inset, height / 2)
                context.closePath()
                context.stroke()
                context.fillStyle = toolbar.accentColor
                context.beginPath()
                context.arc(width / 2, height / 2, dotRadius, 0, Math.PI * 2)
                context.fill()
            }
        }
        Column {
            x: toolbar.identityTextX
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.identityLineGap
            Text {
                objectName: "projectNameLabel"
                width: identity.width - toolbar.identityTextX
                elide: Text.ElideRight
                text: toolbar.bridge.projectName
                color: toolbar.mainTextColor
                font.pixelSize: Theme.typeSubtitle
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "projectStatusLabel"
                width: identity.width - toolbar.identityTextX
                elide: Text.ElideRight
                text: toolbar.statusText
                color: toolbar.statusWarning ? toolbar.amberColor : toolbar.dimTextColor
                font.pixelSize: Theme.typeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: toolbar.statusText
                Accessible.description: toolbar.bridge.autosaveStatus
            }
        }
    }

    // Arrange / Mixer.
    Rectangle {
        id: viewSwitch
        x: identity.x + identity.width + Theme.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        width: viewRow.width + 2 * Theme.spaceXs
        height: toolbar.viewSwitchHeight
        radius: Theme.radiusXl
        color: toolbar.canvasColor
        border.color: toolbar.lineColor
        Row {
            id: viewRow
            x: Theme.spaceXs
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs
            Repeater {
                model: [{ key: "arrange", label: "Arrange" }, { key: "mixer", label: "Mixer" }]
                delegate: Rectangle {
                    id: viewButton
                    required property var modelData
                    readonly property bool current: toolbar.currentView === modelData.key
                    objectName: modelData.key === "arrange" ? "viewArrangeButton" : "viewMixerButton"
                    width: viewLabel.implicitWidth + toolbar.viewButtonPadding
                    height: toolbar.toggleHeight
                    radius: Theme.radiusLg
                    color: current ? Theme.rowHighlight : viewTip.hovered ? Theme.browserChipHover : "transparent"
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
                    border.color: activeFocus ? toolbar.accentColor : "transparent"
                    activeFocusOnTab: true
                    Tip {
                        id: viewTip
                        text: viewButton.modelData.key === "arrange"
                              ? "The arrangement: tracks, clips and the device chain"
                              : "The mixer: every channel's level, pan, sends and meters"
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label + " view"
                    Accessible.checked: current
                    Keys.onReturnPressed: toolbar.viewRequested(modelData.key)
                    Keys.onSpacePressed: toolbar.viewRequested(modelData.key)
                    Text {
                        id: viewLabel
                        anchors.centerIn: parent
                        text: viewButton.modelData.label
                        color: viewButton.current ? toolbar.mainTextColor : toolbar.dimTextColor
                        font.pixelSize: Theme.typeLabel
                        font.weight: viewButton.current ? Font.DemiBold : Font.Medium
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: toolbar.viewRequested(viewButton.modelData.key)
                    }
                }
            }
        }
    }

    // Transport and time.
    Row {
        id: transport
        x: viewSwitch.x + viewSwitch.width + toolbar.transportGap
        anchors.verticalCenter: parent.verticalCenter
        spacing: toolbar.transportSpacing
        Rectangle {
            id: stopButton
            objectName: "stopPlayback"
            anchors.verticalCenter: parent.verticalCenter
            width: toolbar.transportButtonWidth
            height: toolbar.transportButtonHeight
            radius: Theme.radiusXl
            color: stopTip.hovered && toolbar.bridge.playbackActive ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten)
                                                                    : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            border.color: activeFocus ? toolbar.accentColor : Theme.fieldEdge
            activeFocusOnTab: toolbar.bridge.playbackActive
            Tip { id: stopTip; text: "Stop playback" }
            Accessible.role: Accessible.Button
            Accessible.name: "Stop playback"
            Keys.onReturnPressed: toolbar.stopRequested()
            Keys.onSpacePressed: toolbar.stopRequested()
            Rectangle {
                anchors.centerIn: parent
                width: toolbar.transportGlyphSize
                height: toolbar.transportGlyphSize
                radius: Theme.radiusXs
                color: toolbar.bridge.playbackActive ? Theme.textSoft : Theme.transportGlyphOff
            }
            MouseArea {
                anchors.fill: parent
                enabled: toolbar.bridge.playbackActive
                onClicked: toolbar.stopRequested()
            }
        }
        Rectangle {
            id: transportPill
            objectName: "togglePlayback"
            anchors.verticalCenter: parent.verticalCenter
            width: toolbar.playButtonWidth
            height: toolbar.transportButtonHeight
            radius: Theme.radiusXl
            readonly property bool usable: toolbar.bridge.playbackEnabled
                && toolbar.bridge.playbackPauseSupported
            color: toolbar.playing ? toolbar.accentColor
                : playTip.hovered && usable ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
            Tip {
                id: playTip
                text: transportPill.Accessible.name
                shortcut: "Space"
            }
            border.color: activeFocus ? toolbar.mainTextColor
                : toolbar.playing ? toolbar.accentColor : Theme.fieldEdge
            opacity: usable ? 1 : Theme.unusableOpacity
            activeFocusOnTab: usable
            Accessible.role: Accessible.Button
            Accessible.name: !toolbar.bridge.playbackPauseSupported
                             ? "Pause unavailable for this instrument path"
                             : toolbar.bridge.playbackPaused ? "Resume playback"
                             : toolbar.bridge.playbackActive ? "Pause playback" : "Start playback"
            Accessible.description: toolbar.statusText
            Keys.onReturnPressed: toolbar.playbackRequested()
            Keys.onSpacePressed: toolbar.playbackRequested()
            Canvas {
                id: playGlyph
                // Pause: two bars; play: a triangle pointing right.
                readonly property real barInset: 1
                readonly property real barWidth: 4
                readonly property real triangleInset: 2
                anchors.centerIn: parent
                width: toolbar.playGlyphSize
                height: toolbar.playGlyphSize
                readonly property bool pause: toolbar.playing
                onPauseChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    if (pause) {
                        context.fillStyle = Theme.canvas
                        context.fillRect(barInset, 0, barWidth, height)
                        context.fillRect(width - barInset - barWidth, 0, barWidth, height)
                    } else {
                        context.fillStyle = toolbar.accentColor
                        context.beginPath()
                        context.moveTo(triangleInset, 0)
                        context.lineTo(width, height / 2)
                        context.lineTo(triangleInset, height)
                        context.closePath()
                        context.fill()
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                enabled: transportPill.usable
                onClicked: toolbar.playbackRequested()
            }
        }
        // Record (ADR-0086): arms a take; from stop it starts the song, after
        // a bar of count-in when that is on.
        Rectangle {
            id: recordButton
            objectName: "recordButton"
            readonly property bool armed: toolbar.recording.armed
            readonly property bool live: toolbar.recording.recording
            anchors.verticalCenter: parent.verticalCenter
            width: toolbar.transportButtonWidth
            height: toolbar.transportButtonHeight
            radius: Theme.radiusXl
            color: armed ? Qt.rgba(toolbar.recordColor.r, toolbar.recordColor.g, toolbar.recordColor.b, Theme.recordArmedAlpha)
                : recordTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
            border.color: activeFocus ? toolbar.mainTextColor : armed ? toolbar.recordColor : Theme.fieldEdge
            activeFocusOnTab: true
            Tip {
                id: recordTip
                text: recordButton.armed ? "Stop recording (the song plays on)"
                    : "Record: play the computer keyboard or the piano roll's keys and the notes are written into the track"
                shortcut: "Ctrl+R"
            }
            Accessible.role: Accessible.Button
            Accessible.name: armed ? "Stop recording" : "Record"
            Keys.onReturnPressed: toolbar.recording.toggle_record()
            Keys.onSpacePressed: toolbar.recording.toggle_record()
            Rectangle {
                anchors.centerIn: parent
                width: toolbar.transportGlyphSize
                height: toolbar.transportGlyphSize
                radius: Theme.radiusLg
                color: toolbar.recordColor
                opacity: recordButton.armed && !recordButton.live ? recordBlink.value : 1
                QtObject {
                    id: recordBlink
                    property real value: 1
                }
                SequentialAnimation {
                    running: recordButton.armed && !recordButton.live && Motion.enabled
                    loops: Animation.Infinite
                    NumberAnimation { target: recordBlink; property: "value"; to: Theme.recordBlinkLow; duration: Motion.slow }
                    NumberAnimation { target: recordBlink; property: "value"; to: 1; duration: Motion.slow }
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: toolbar.recording.toggle_record()
            }
        }
        Text {
            objectName: "countInReadout"
            anchors.verticalCenter: parent.verticalCenter
            visible: toolbar.recording.countingIn
            text: "count-in"
            color: toolbar.recordColor
            font.pixelSize: Theme.typeBody
            font.family: toolbar.monoFamily
        }
        // The click while playing, and a bar of count-in before a take.
        Repeater {
            model: [
                {name: "metronomeToggle", label: "♩ Click", key: "click",
                 tip: "The metronome: a click on every beat while recording (never in an export)"},
                {name: "countInToggle", label: "Count-in", key: "countIn",
                 tip: "Before recording from stop, a bar of clicks to count you in"}
            ]
            delegate: Rectangle {
                id: recordOption
                required property var modelData
                readonly property bool on: toolbar.recording[modelData.key]
                objectName: modelData.name
                anchors.verticalCenter: parent.verticalCenter
                width: optionLabel.implicitWidth + toolbar.togglePadding
                height: toolbar.toggleHeight
                radius: toolbar.toggleRadius
                color: on ? Qt.rgba(toolbar.amberColor.r, toolbar.amberColor.g, toolbar.amberColor.b, Theme.amberToggleAlpha)
                    : optionTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
                border.color: on ? toolbar.amberColor : toolbar.lineColor
                Tip { id: optionTip; text: recordOption.modelData.tip }
                Accessible.role: Accessible.CheckBox
                Accessible.name: modelData.label
                Accessible.checked: on
                Text {
                    id: optionLabel
                    anchors.centerIn: parent
                    text: recordOption.modelData.label
                    color: recordOption.on ? toolbar.amberColor : toolbar.dimTextColor
                    font.pixelSize: Theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: toolbar.recording[recordOption.modelData.key] = !recordOption.on
                }
            }
        }
        LoopControls {
            objectName: "loopControls"
            anchors.verticalCenter: parent.verticalCenter
            transport: toolbar.bridge.loopTransport
            leadBars: toolbar.bridge.leadBars
            raisedColor: toolbar.raisedColor
            lineColor: toolbar.lineColor
            mainTextColor: toolbar.mainTextColor
            dimTextColor: toolbar.dimTextColor
            accentColor: toolbar.accentColor
            amberColor: toolbar.amberColor
        }
        // The computer keyboard plays the selected instrument (Ctrl+Shift+K).
        Rectangle {
            id: keyboardToggle
            objectName: "computerKeyboardToggle"
            readonly property var keyboard: toolbar.bridge.computerKeyboard
            readonly property bool on: keyboard.enabled
            anchors.verticalCenter: parent.verticalCenter
            width: keyboardLabel.implicitWidth + toolbar.keysTogglePadding
            height: toolbar.toggleHeight
            radius: toolbar.toggleRadius
            color: on ? Qt.rgba(toolbar.accentColor.r, toolbar.accentColor.g, toolbar.accentColor.b, Theme.accentToggleAlpha)
                : keysTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
            border.color: on ? toolbar.accentColor : toolbar.lineColor
            Tip {
                id: keysTip
                text: "Play the selected instrument from the computer keyboard (A W S E D F…)"
                shortcut: "Ctrl+Shift+K"
            }
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Play the selected instrument from the computer keyboard"
            Accessible.checked: on
            Canvas {
                id: keyboardIcon
                x: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                // A key outline with key gaps along its top edge.
                readonly property real pixelCentre: 0.5
                readonly property real firstGap: 4
                readonly property real gapPitch: 3.5
                readonly property real gapDepth: 6
                width: toolbar.keysIconWidth
                height: toolbar.keysIconHeight
                onPaint: {
                    const context = getContext("2d")
                    context.reset()
                    context.strokeStyle = keyboardToggle.on ? toolbar.accentColor : toolbar.dimTextColor
                    context.lineWidth = Theme.toolbarIconStroke
                    context.strokeRect(pixelCentre, pixelCentre, width - 1, height - 1)
                    for (let x = firstGap; x < width - 1; x += gapPitch) {
                        context.beginPath()
                        context.moveTo(x, pixelCentre)
                        context.lineTo(x, gapDepth)
                        context.stroke()
                    }
                }
                Connections {
                    target: keyboardToggle
                    function onOnChanged() { keyboardIcon.requestPaint() }
                }
            }
            Text {
                id: keyboardLabel
                x: toolbar.keysLabelX
                anchors.verticalCenter: parent.verticalCenter
                text: keyboardToggle.on ? keyboardToggle.keyboard.range + "  ·  vel " + keyboardToggle.keyboard.velocity
                                        : "Keys"
                color: keyboardToggle.on ? toolbar.accentColor : toolbar.dimTextColor
                font.pixelSize: Theme.typeBody
                font.family: keyboardToggle.on ? toolbar.monoFamily : font.family
            }
            MouseArea {
                anchors.fill: parent
                onClicked: keyboardToggle.keyboard.toggle()
            }
        }
        // In key: the keyboard plays only the project key's notes.
        Rectangle {
            id: inKeyToggle
            objectName: "computerKeyboardInKey"
            readonly property var keyboard: toolbar.bridge.computerKeyboard
            readonly property bool on: keyboard.inKey
            readonly property bool hasKey: keyboard.scaleName.length > 0
            visible: keyboard.enabled
            anchors.verticalCenter: parent.verticalCenter
            width: inKeyLabel.implicitWidth + toolbar.togglePadding
            height: toolbar.toggleHeight
            radius: toolbar.toggleRadius
            color: on ? Qt.rgba(toolbar.accentColor.r, toolbar.accentColor.g, toolbar.accentColor.b, Theme.accentToggleAlpha)
                : inKeyTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
            border.color: on ? toolbar.accentColor : toolbar.lineColor
            Tip {
                id: inKeyTip
                text: inKeyToggle.hasKey
                    ? "Play only " + inKeyToggle.keyboard.scaleName + ": the home row walks up the scale, the row above is an octave higher"
                    : "Set the project key to play in key"
            }
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Play the computer keyboard in the project key"
            Accessible.checked: on
            Text {
                id: inKeyLabel
                anchors.centerIn: parent
                text: inKeyToggle.on && !inKeyToggle.hasKey ? "In key · no key set" : "In key"
                color: inKeyToggle.on ? (inKeyToggle.hasKey ? toolbar.accentColor : toolbar.amberColor)
                                      : toolbar.dimTextColor
                font.pixelSize: Theme.typeBody
            }
            MouseArea {
                anchors.fill: parent
                onClicked: inKeyToggle.keyboard.toggle_in_key()
            }
        }
        Rectangle {
            id: readouts
            anchors.verticalCenter: parent.verticalCenter
            width: readoutRow.width + Theme.spaceLg
            height: toolbar.readoutsHeight
            radius: Theme.radiusXl
            color: toolbar.canvasColor
            border.color: toolbar.lineColor
            Row {
                id: readoutRow
                x: Theme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                Column {
                    objectName: "positionReadout"
                    width: toolbar.positionReadoutWidth
                    leftPadding: toolbar.readoutPadding
                    Text { text: "POSITION"; color: toolbar.dimTextColor; font.pixelSize: Theme.typeSmall; font.letterSpacing: Theme.trackingSm }
                    Text {
                        text: toolbar.positionText
                        color: toolbar.mainTextColor
                        font.pixelSize: Theme.typeHeading
                        font.family: toolbar.monoFamily
                        Accessible.role: Accessible.StaticText
                        Accessible.name: "Position bar " + toolbar.positionText
                    }
                }
                Rectangle { width: Theme.hairline; height: toolbar.readoutDividerHeight; color: toolbar.lineColor; anchors.verticalCenter: parent.verticalCenter }
                ProjectGlobalControls {
                    id: projectTiming
                    anchors.verticalCenter: parent.verticalCenter
                    bridge: toolbar.bridge
                    panelColor: toolbar.canvasColor
                    raisedColor: toolbar.raisedColor
                    lineColor: toolbar.lineColor
                    mainTextColor: toolbar.mainTextColor
                    dimTextColor: toolbar.dimTextColor
                    accentColor: toolbar.accentColor
                    amberColor: toolbar.amberColor
                }
                Rectangle {
                    width: Theme.hairline; height: toolbar.readoutDividerHeight; color: toolbar.lineColor
                    anchors.verticalCenter: parent.verticalCenter
                }
                // The song's key: click to choose it.
                Item {
                    id: keyReadout
                    objectName: "keyReadout"
                    width: toolbar.keyReadoutWidth
                    height: keyColumn.implicitHeight
                    anchors.verticalCenter: parent.verticalCenter
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Project key " + (toolbar.bridge.projectKey || "not set")
                    Keys.onReturnPressed: toolbar.keyPickerOpen = !toolbar.keyPickerOpen
                    Keys.onSpacePressed: toolbar.keyPickerOpen = !toolbar.keyPickerOpen
                    Column {
                        id: keyColumn
                        leftPadding: toolbar.readoutPadding
                        Text { text: "KEY"; color: toolbar.dimTextColor; font.pixelSize: Theme.typeSmall; font.letterSpacing: Theme.trackingSm }
                        Text {
                            text: toolbar.bridge.projectKey.length > 0 ? toolbar.bridge.projectKey : "—"
                            color: keyReadout.activeFocus || keyArea.containsMouse ? toolbar.accentColor
                                : toolbar.mainTextColor
                            font.pixelSize: Theme.typeHeading
                            font.family: toolbar.monoFamily
                        }
                    }
                    MouseArea {
                        id: keyArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: toolbar.keyPickerOpen = !toolbar.keyPickerOpen
                        Tip {
                            anchorItem: keyReadout
                            text: "The song's key: scale tools, chords and key-matched drops follow it"
                        }
                    }
                }
            }
        }
    }

    // Master output, CPU and project actions.
    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXl
        anchors.verticalCenter: parent.verticalCenter
        spacing: toolbar.actionsSpacing
        Column {
            objectName: "masterOutputReadout"
            anchors.verticalCenter: parent.verticalCenter
            Tip { text: "Master short-term loudness and true peak; the mixer's master section has more" }
            Text {
                anchors.right: parent.right
                text: "MASTER"
                color: toolbar.dimTextColor
                font.pixelSize: Theme.typeSmall
                font.letterSpacing: Theme.trackingSm
            }
            Text {
                anchors.right: parent.right
                text: toolbar.masterText
                color: toolbar.bridge.masterTruePeak.incomplete ? toolbar.amberColor : toolbar.mainTextColor
                font.pixelSize: Theme.typeLabel
                font.family: toolbar.monoFamily
                Accessible.role: Accessible.StaticText
                Accessible.name: "Master output " + toolbar.masterText
            }
            // Live master level: RMS bars with the peak as a tick.
            Item {
                id: masterMeter
                objectName: "masterMeter"
                anchors.right: parent.right
                width: toolbar.meterWidth
                height: toolbar.meterHeight
                readonly property var reading: toolbar.bridge.meters.channel("")
                Accessible.name: "Master stereo output meter"
                Accessible.description: !reading.valid ? "Stopped"
                    : (reading.clipped ? "Clipping. " : "")
                      + "Left peak " + Number(reading.leftPeak).toFixed(2)
                      + ", right peak " + Number(reading.rightPeak).toFixed(2)
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        id: channel
                        required property int index
                        readonly property real rms: index === 0 ? masterMeter.reading.leftRms : masterMeter.reading.rightRms
                        readonly property real peak: index === 0 ? masterMeter.reading.leftPeak : masterMeter.reading.rightPeak
                        y: Theme.spaceXxs + index * toolbar.meterBarPitch
                        width: masterMeter.width
                        height: toolbar.meterBarHeight
                        radius: height / 2
                        color: Theme.meterTrack
                        Rectangle {
                            width: Math.min(1, Math.max(0, channel.rms)) * parent.width
                            height: parent.height
                            radius: height / 2
                            color: channel.rms >= toolbar.meterHotLevel ? Theme.masterMeterHot : channel.rms >= toolbar.meterWarmLevel ? Theme.amber : Theme.accent
                        }
                        Rectangle {
                            visible: masterMeter.reading.valid && channel.peak > 0
                            x: Math.min(1, Math.max(0, channel.peak)) * (parent.width - toolbar.meterPeakWidth)
                            width: toolbar.meterPeakWidth
                            height: parent.height
                            color: masterMeter.reading.clipped || channel.peak >= toolbar.meterHotLevel ? Theme.meterPeakHot : Theme.textMain
                        }
                    }
                }
            }
        }
        Column {
            objectName: "cpuReadout"
            anchors.verticalCenter: parent.verticalCenter
            visible: toolbar.bridge.cpuLoad !== undefined
            Tip { text: "How much of each audio block the engine needs; above 80% risks dropouts" }
            Text {
                objectName: "cpuThreadsLabel"
                anchors.right: parent.right
                text: toolbar.bridge.audioThreads > 1 ? "CPU ×" + toolbar.bridge.audioThreads : "CPU"
                color: toolbar.dimTextColor
                font.pixelSize: Theme.typeSmall
                font.letterSpacing: Theme.trackingSm
            }
            Text {
                anchors.right: parent.right
                text: toolbar.bridge.cpuLoad !== undefined
                      ? Math.round(Number(toolbar.bridge.cpuLoad) * 100) + "%" : ""
                color: toolbar.mainTextColor
                font.pixelSize: Theme.typeLabel
                font.family: toolbar.monoFamily
            }
        }
        // Mix Check: a full view; lit while it is shown, "Checking…" while it runs.
        Rectangle {
            id: mixCheckButton
            objectName: "mixCheckToolbarButton"
            readonly property bool current: toolbar.currentView === "mixcheck"
            readonly property bool running: toolbar.bridge.mixCheck ? toolbar.bridge.mixCheck.running === true : false
            anchors.verticalCenter: parent.verticalCenter
            width: mixCheckLabel.implicitWidth + toolbar.actionButtonPadding
            height: toolbar.actionButtonHeight
            radius: Theme.radiusXl
            color: current ? Qt.rgba(toolbar.accentColor.r, toolbar.accentColor.g, toolbar.accentColor.b, Theme.accentToggleAlpha)
                : mixCheckTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Tip {
                id: mixCheckTip
                text: "Mix Check: find what pokes out in the mix, the track to blame and a fix"
                shortcut: "Ctrl+Shift+M"
            }
            border.color: activeFocus ? toolbar.accentColor : current ? toolbar.accentColor : Theme.fieldEdge
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Mix Check"
            Accessible.checked: current
            Keys.onReturnPressed: toolbar.mixCheckRequested()
            Keys.onSpacePressed: toolbar.mixCheckRequested()
            Text {
                id: mixCheckLabel
                anchors.centerIn: parent
                text: mixCheckButton.running ? "Checking…" : "Mix Check"
                color: mixCheckButton.current ? toolbar.accentColor : Theme.textSoft
                font.pixelSize: Theme.typeLabel
            }
            MouseArea {
                anchors.fill: parent
                onClicked: toolbar.mixCheckRequested()
            }
        }
        Rectangle {
            id: exportButton
            objectName: "toolbarExportButton"
            anchors.verticalCenter: parent.verticalCenter
            width: exportLabel.implicitWidth + toolbar.actionButtonPadding
            height: toolbar.actionButtonHeight
            radius: Theme.radiusXl
            color: exportTip.hovered ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten) : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Tip { id: exportTip; text: "Export the mix: file, format and rate"; shortcut: "Ctrl+Shift+E" }
            border.color: activeFocus ? toolbar.accentColor : Theme.fieldEdge
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Export"
            Keys.onReturnPressed: toolbar.exportRequested()
            Keys.onSpacePressed: toolbar.exportRequested()
            Text {
                id: exportLabel
                anchors.centerIn: parent
                text: toolbar.bridge.exports.exportBusy ? "Exporting…" : "Export…"
                color: Theme.textSoft
                font.pixelSize: Theme.typeLabel
            }
            MouseArea {
                anchors.fill: parent
                onClicked: toolbar.exportRequested()
            }
        }
        Rectangle {
            id: compactMore
            objectName: "toolbarMoreButton"
            anchors.verticalCenter: parent.verticalCenter
            width: toolbar.moreButtonWidth
            height: toolbar.actionButtonHeight
            radius: Theme.radiusXl
            color: moreTip.hovered || toolbar.compactMenuOpen ? Qt.lighter(toolbar.raisedColor, Theme.hoverLighten)
                                                               : toolbar.raisedColor
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Tip { id: moreTip; text: "Inspector, master effects, plug-ins, media, versions, audio output and more" }
            border.color: activeFocus || toolbar.compactMenuOpen ? toolbar.accentColor : Theme.fieldEdge
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "Project and audio settings"
            Accessible.description: "Inspector, master effects, plug-ins, media, versions, audio output and more"
            Keys.onReturnPressed: toolbar.compactMenuOpen = !toolbar.compactMenuOpen
            Keys.onSpacePressed: toolbar.compactMenuOpen = !toolbar.compactMenuOpen
            Text {
                anchors.centerIn: parent
                text: "More ▾"
                color: Theme.textSoft
                font.pixelSize: Theme.typeLabel
            }
            MouseArea {
                anchors.fill: parent
                onClicked: toolbar.compactMenuOpen = !toolbar.compactMenuOpen
            }
        }
    }

    // Escape or a click anywhere else closes the open menu.
    MouseArea {
        parent: toolbar.parent
        anchors.fill: parent
        z: 99
        visible: toolbar.visible && (toolbar.compactMenuOpen || toolbar.keyPickerOpen)
        onPressed: {
            toolbar.compactMenuOpen = false
            toolbar.keyPickerOpen = false
        }
    }
    Rectangle {
        id: compactMenu
        objectName: "toolbarMoreMenu"
        parent: toolbar.parent
        x: toolbar.x + toolbar.width - width - Theme.spaceXl
        y: toolbar.y + toolbar.height - 1
        z: 100
        visible: toolbar.visible && toolbar.compactMenuOpen
        onVisibleChanged: if (visible) forceActiveFocus()
        transformOrigin: Item.TopRight
        Reveal { target: compactMenu; fromScale: Motion.popScale }
        Keys.onEscapePressed: toolbar.compactMenuOpen = false
        width: toolbar.menuWidth
        height: menuColumn.implicitHeight + 2 * Theme.spaceXs
        radius: Theme.radiusXl
        color: toolbar.raisedColor
        border.color: toolbar.lineColor
        MouseArea { anchors.fill: parent }

        Column {
            id: menuColumn
            x: Theme.spaceXs
            y: Theme.spaceXs
            Repeater {
                model: toolbar.menuItems
                delegate: Rectangle {
                    id: menuItem
                    required property var modelData
                    objectName: modelData.name
                    width: compactMenu.width - 2 * Theme.spaceXs
                    height: toolbar.menuRowHeight
                    radius: Theme.controlRadius
                    color: activeFocus || itemArea.containsMouse ? toolbar.panelColor : "transparent"
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    activeFocusOnTab: compactMenu.visible
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label
                    Keys.onReturnPressed: toolbar.activateMenuItem(modelData.key)
                    Keys.onSpacePressed: toolbar.activateMenuItem(modelData.key)
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spaceLg
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 2 * Theme.spaceLg
                        elide: Text.ElideRight
                        text: menuItem.modelData.key === "media" && toolbar.bridge.managedMedia.managedMediaIssues.length > 0
                              ? "Media  ! " + toolbar.bridge.managedMedia.managedMediaIssues.length : menuItem.modelData.label
                        color: menuItem.modelData.key === "media" && toolbar.bridge.managedMedia.managedMediaIssues.length > 0
                               ? toolbar.amberColor : toolbar.mainTextColor
                        font.pixelSize: Theme.typeLabel
                    }
                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: toolbar.activateMenuItem(menuItem.modelData.key)
                    }
                }
            }
        }
    }

    // The key picker: the twelve tonics, major or minor, the key detected in
    // the project's audio, and clear.
    Rectangle {
        id: keyPicker
        objectName: "keyPicker"
        parent: toolbar.parent
        property bool minor: toolbar.bridge.projectKey.endsWith("min")
        readonly property var tonics: Contracts.pitch_class_names(Contracts.ascii)
        x: Math.min(toolbar.parent.width - width - Theme.spaceMd,
                    keyReadout.mapToItem(toolbar.parent, 0, 0).x - Theme.spaceMd)
        y: toolbar.y + toolbar.height - 1
        z: 100
        visible: toolbar.visible && toolbar.keyPickerOpen
        onVisibleChanged: {
            if (!visible)
                return
            minor = toolbar.bridge.projectKey.endsWith("min")
            forceActiveFocus()
        }
        Keys.onEscapePressed: toolbar.keyPickerOpen = false
        transformOrigin: Item.Top
        Reveal { target: keyPicker; fromScale: Motion.popScale }
        width: toolbar.keyPickerWidth
        height: pickerColumn.implicitHeight + 2 * Theme.spaceMd
        radius: Theme.radiusXl
        color: toolbar.raisedColor
        border.color: toolbar.lineColor
        MouseArea { anchors.fill: parent }
        component PickerButton: Rectangle {
            id: pickerButton
            property string label: ""
            property bool current: false
            signal chosen()
            height: toolbar.keyPickerRowHeight
            radius: Theme.controlRadius
            color: current ? Theme.rowHighlight : pickerArea.containsMouse || activeFocus ? toolbar.panelColor : "transparent"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            border.color: current ? Theme.lineStrong : "transparent"
            activeFocusOnTab: keyPicker.visible
            Accessible.role: Accessible.Button
            Accessible.name: label
            Keys.onReturnPressed: chosen()
            Keys.onSpacePressed: chosen()
            Text {
                anchors.centerIn: parent
                text: pickerButton.label
                color: pickerButton.current ? toolbar.mainTextColor : Theme.textSoft
                font.pixelSize: Theme.typeLabel
                font.weight: pickerButton.current ? Font.DemiBold : Font.Normal
            }
            MouseArea { id: pickerArea; anchors.fill: parent; hoverEnabled: true; onClicked: pickerButton.chosen() }
        }
        Column {
            id: pickerColumn
            x: Theme.spaceMd
            y: Theme.spaceMd
            width: parent.width - 2 * Theme.spaceMd
            spacing: Theme.spaceSm
            Row {
                spacing: Theme.spaceXs
                Repeater {
                    model: ["Major", "Minor"]
                    delegate: PickerButton {
                        required property string modelData
                        width: (pickerColumn.width - Theme.spaceXs) / 2
                        label: modelData
                        current: (modelData === "Minor") === keyPicker.minor
                        onChosen: keyPicker.minor = modelData === "Minor"
                    }
                }
            }
            Grid {
                columns: toolbar.keyPickerColumns
                spacing: Theme.spaceXs
                Repeater {
                    model: keyPicker.tonics
                    delegate: PickerButton {
                        required property string modelData
                        required property int index
                        objectName: "keyPickerTonic:" + modelData
                        width: (pickerColumn.width - (toolbar.keyPickerColumns - 1) * Theme.spaceXs) / toolbar.keyPickerColumns
                        label: modelData
                        current: toolbar.bridge.projectKey === modelData + (keyPicker.minor ? " min" : " maj")
                        onChosen: {
                            toolbar.bridge.set_project_key(index, keyPicker.minor ? "minor" : "major")
                            toolbar.keyPickerOpen = false
                        }
                    }
                }
            }
            PickerButton {
                objectName: "keyPickerSuggested"
                visible: toolbar.bridge.suggestedProjectKey.length > 0
                width: pickerColumn.width
                label: "Use " + toolbar.bridge.suggestedProjectKey + " · detected in your audio"
                onChosen: { toolbar.bridge.use_suggested_project_key(); toolbar.keyPickerOpen = false }
            }
            PickerButton {
                objectName: "keyPickerClear"
                visible: toolbar.bridge.projectKey.length > 0
                width: pickerColumn.width
                label: "Clear the key"
                onChosen: { toolbar.bridge.clear_project_key(); toolbar.keyPickerOpen = false }
            }
        }
    }
}
