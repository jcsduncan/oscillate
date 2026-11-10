pragma ComponentBehavior: Bound
import QtQuick
import "assistant"
import "shell"

// The master assistant (ADR-0124), opened from the master: choose a
// reference (a genre target or reference tracks), a loudness target and a
// character; Analyse measures the mix entering the master chain and plans a
// chain of the project's own devices. Each proposed device says why in one
// line, and each can be kept or left out. The A/B plays the loudest stretch
// through the current master and through the proposal at matched loudness.
// Accept is one undo step; Reject leaves nothing behind. `controller` is
// projectBridge.masterAssistant.
Rectangle {
    id: view
    objectName: "masterAssistantView"
    required property var controller

    // The mixer, from the header's button.
    signal closeRequested()

    readonly property AssistantTheme style: AssistantTheme {}
    readonly property var c: controller
    readonly property bool ready: c !== null
    readonly property string phase: ready ? c.phase : "idle"
    readonly property bool working: phase === "working"
    readonly property var measured: ready ? c.measured : ({})
    readonly property bool hasMeasure: measured.integrated !== undefined

    color: style.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Master assistant"
    // Leaving the view silences the A/B.
    Component.onDestruction: if (view.c !== null) view.c.stopAudition()

    // ---- Header ----------------------------------------------------------------------
    Item {
        id: header
        x: view.style.viewMargin
        y: view.style.viewMargin
        width: view.width - 2 * view.style.viewMargin
        height: view.style.headerHeight
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: view.style.spaceXxs
            Text {
                objectName: "assistantTitle"
                text: "Master Assistant"
                color: view.style.textMain
                font.family: view.style.sansFamily
                font.pixelSize: view.style.titleSize
                font.weight: Font.ExtraBold
                font.letterSpacing: view.style.titleSpacing
            }
            Text {
                text: "A master chain of your own devices, toward a reference and a loudness target. Nothing changes until you accept"
                color: view.style.textDim
                font.pixelSize: view.style.typeBody
            }
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: view.style.spaceLg
            Text {
                objectName: "assistantStatus"
                anchors.verticalCenter: parent.verticalCenter
                text: view.ready ? (view.working ? view.c.stage : view.c.status) : ""
                color: view.style.textDim
                font.pixelSize: view.style.typeSmall
            }
            HeaderButton {
                objectName: "assistantAnalyse"
                label: view.working ? "Stop" : (view.ready && view.c.stale ? "Analyse again" : "Analyse")
                accent: !view.working
                tip: "Measure the mix as it enters the master chain and propose a chain"
                Accessible.name: view.working ? "Stop the analysis" : "Analyse the mix"
                onClicked: view.working ? view.c.cancel() : view.c.analyse()
            }
            HeaderButton {
                objectName: "assistantClose"
                label: "Mixer"
                tip: "Back to the mixer (Esc)"
                Accessible.name: "Back to the mixer"
                onClicked: view.closeRequested()
            }
        }
    }
    Rectangle {
        objectName: "assistantProgress"
        x: header.x
        y: header.y + header.height + view.style.spaceXs
        width: header.width * (view.ready ? view.c.progress : 0)
        height: view.style.progressHeight
        radius: height / 2
        color: view.style.accent
        visible: view.working
    }

    // A header action.
    component HeaderButton: Rectangle {
        id: button
        property string label
        property string tip
        property bool accent: false
        signal clicked()
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: buttonLabel.implicitWidth + view.style.buttonPadding
        height: view.style.buttonHeight
        radius: view.style.controlRadius
        color: accent ? view.style.accent : (buttonArea.containsMouse ? view.style.buttonFace : view.style.panelRaised)
        border.color: accent ? view.style.accent : view.style.lineStrong
        opacity: enabled ? 1 : view.style.droppedOpacity
        activeFocusOnTab: true
        Keys.onReturnPressed: button.clicked()
        Keys.onEnterPressed: button.clicked()
        Keys.onSpacePressed: button.clicked()
        Accessible.role: Accessible.Button
        FocusRing { targetRadius: view.style.controlRadius }
        Text {
            id: buttonLabel
            anchors.centerIn: parent
            text: button.label
            color: button.accent ? view.style.onAccentText : view.style.textMain
            font.pixelSize: view.style.typeBody
            font.bold: button.accent
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
            Tip { text: button.tip }
        }
    }

    // A card's caption.
    component Caption: Text {
        color: view.style.textMain
        font.pixelSize: view.style.typeSmall
        font.bold: true
        font.letterSpacing: view.style.trackingXl
    }

    // A line of words in a card.
    component Line: Text {
        color: view.style.textDim
        font.pixelSize: view.style.typeBody
        wrapMode: Text.WordWrap
    }

    // ---- Goals -----------------------------------------------------------------------
    Rectangle {
        id: goals
        objectName: "assistantGoals"
        x: view.style.viewMargin
        anchors.top: header.bottom
        anchors.topMargin: view.style.sectionGap
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.style.viewMargin
        width: view.style.goalsWidth
        radius: view.style.cardRadius
        color: view.style.panel
        border.color: view.style.line
        Flickable {
            anchors.fill: parent
            anchors.margins: view.style.cardPadding
            contentHeight: goalColumn.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Column {
                id: goalColumn
                width: parent.width
                spacing: view.style.rowGap
                Caption { text: "REFERENCE" }
                AssistantChips {
                    width: parent.width
                    style: view.style
                    prefix: "assistantGenre:"
                    model: view.ready ? view.c.genres : []
                    current: view.ready && view.c.references.length === 0 ? view.c.genre : ""
                    tip: "Master toward what finished records in this genre measure"
                    onChosen: id => view.c.genre = id
                }
                Repeater {
                    model: view.ready ? view.c.references : []
                    delegate: Item {
                        id: referenceRow
                        required property var modelData
                        required property int index
                        objectName: "assistantReference:" + index
                        width: goalColumn.width
                        height: view.style.chipHeight
                        Text {
                            anchors.left: parent.left
                            anchors.right: removeReference.left
                            anchors.rightMargin: view.style.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            text: referenceRow.modelData.name
                            elide: Text.ElideRight
                            property bool tipShowsWhole: true
                            color: view.style.textMain
                            font.pixelSize: view.style.typeBody
                        }
                        Text {
                            id: removeReference
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Remove"
                            color: view.style.textDim
                            font.pixelSize: view.style.typeSmall
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: view.c.removeReference(referenceRow.index)
                            }
                        }
                    }
                }
                HeaderButton {
                    objectName: "assistantAddReference"
                    anchors.verticalCenter: undefined
                    label: "Add reference tracks…"
                    tip: "Master toward finished tracks you choose: their balance, width and crest"
                    Accessible.name: "Add reference tracks"
                    onClicked: view.c.chooseReferences()
                }

                Caption { text: "LOUDNESS" }
                AssistantChips {
                    width: parent.width
                    style: view.style
                    prefix: "assistantLoudness:"
                    model: {
                        const out = []
                        const targets = view.ready ? view.c.loudnessTargets : []
                        for (let i = 0; i < targets.length; ++i)
                            out.push({ id: targets[i].id,
                                       name: targets[i].id === "custom" ? "Custom"
                                             : targets[i].name + " " + targets[i].lufs })
                        return out
                    }
                    current: view.ready ? view.c.loudness : ""
                    tip: "The integrated loudness the master lands on, under its true-peak ceiling"
                    onChosen: id => view.c.loudness = id
                }
                Row {
                    visible: view.ready && view.c.loudness === "custom"
                    spacing: view.style.spaceMd
                    HeaderButton {
                        anchors.verticalCenter: undefined
                        label: "−1"
                        tip: "A decibel quieter"
                        Accessible.name: "Quieter target"
                        onClicked: view.c.customLufs = view.c.customLufs - 1
                    }
                    Text {
                        objectName: "assistantCustomLufs"
                        height: view.style.buttonHeight
                        verticalAlignment: Text.AlignVCenter
                        text: (view.ready ? view.c.customLufs.toFixed(1) : "") + " LUFS"
                        color: view.style.textMain
                        font.family: view.style.monoFamily
                        font.pixelSize: view.style.typeBody
                    }
                    HeaderButton {
                        anchors.verticalCenter: undefined
                        label: "+1"
                        tip: "A decibel louder"
                        Accessible.name: "Louder target"
                        onClicked: view.c.customLufs = view.c.customLufs + 1
                    }
                }
                Line {
                    width: parent.width
                    text: view.ready ? "Lands at " + view.c.targetLufs.toFixed(1) + " LUFS, true peak under "
                                       + view.c.targetCeiling.toFixed(1) + " dBTP" : ""
                }

                Caption { text: "CHARACTER" }
                AssistantChips {
                    width: parent.width
                    style: view.style
                    prefix: "assistantCharacter:"
                    model: view.ready ? view.c.characters : []
                    current: view.ready ? view.c.character : ""
                    tip: "Clean adds no colour; Punchy lets the hits through; Warm adds harmonics and glue"
                    onChosen: id => view.c.character = id
                }

                Caption { text: "RANGE" }
                AssistantChips {
                    width: parent.width
                    style: view.style
                    prefix: "assistantRange:"
                    model: view.ready && view.c.loopAvailable
                           ? [{ id: "song", name: "Whole song" }, { id: "loop", name: "Loop" }]
                           : [{ id: "song", name: "Whole song" }]
                    current: view.ready && view.c.useLoop ? "loop" : "song"
                    tip: "Measure the whole song, or only the loop's bars"
                    onChosen: id => view.c.useLoop = id === "loop"
                }
            }
        }
    }

    // ---- Measurements ----------------------------------------------------------------
    Rectangle {
        id: measurements
        objectName: "assistantMeasured"
        anchors.left: goals.right
        anchors.leftMargin: view.style.sectionGap
        anchors.right: proposal.left
        anchors.rightMargin: view.style.sectionGap
        anchors.top: goals.top
        anchors.bottom: goals.bottom
        radius: view.style.cardRadius
        color: view.style.panel
        border.color: view.style.line
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * view.style.cardPadding
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: !view.hasMeasure
            text: "Choose a reference and a target, then Analyse"
            color: view.style.textFaint
            font.pixelSize: view.style.typeBody
        }
        Flickable {
            anchors.fill: parent
            anchors.margins: view.style.cardPadding
            contentHeight: measureColumn.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            visible: view.hasMeasure
            Column {
                id: measureColumn
                width: parent.width
                spacing: view.style.rowGap
                Caption { text: "THE MIX ENTERING THE MASTER" }
                Flow {
                    width: parent.width
                    spacing: view.style.spaceMd
                    Repeater {
                        model: [
                            { id: "integrated", label: "Integrated", unit: "LUFS" },
                            { id: "shortTerm", label: "Short-term max", unit: "LUFS" },
                            { id: "momentary", label: "Momentary max", unit: "LUFS" },
                            { id: "truePeak", label: "True peak", unit: "dBTP" },
                            { id: "crest", label: "Crest", unit: "dB" }
                        ]
                        delegate: Rectangle {
                            id: readout
                            required property var modelData
                            objectName: "assistantReadout:" + modelData.id
                            width: view.style.readoutWidth
                            height: view.style.readoutHeight
                            radius: view.style.radiusMd
                            color: view.style.well
                            border.color: view.style.wellEdge
                            Column {
                                anchors.centerIn: parent
                                spacing: 0
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: view.measured[readout.modelData.id] !== undefined
                                          ? view.measured[readout.modelData.id] : "–"
                                    color: view.style.textMain
                                    font.family: view.style.monoFamily
                                    font.pixelSize: view.style.typeHeading
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: readout.modelData.label + " · " + readout.modelData.unit
                                    color: view.style.textFaint
                                    font.pixelSize: view.style.typeMicro
                                }
                            }
                        }
                    }
                }
                Line {
                    objectName: "assistantTonalCaption"
                    width: parent.width
                    text: view.measured.tonalDistance !== undefined
                          ? "Tonal balance against " + view.c.referenceName + ": "
                            + view.measured.tonalDistance + " dB RMS from its median; the band shows its middle half"
                          : ""
                }
                AssistantTonal {
                    objectName: "assistantTonal"
                    width: parent.width
                    height: view.style.tonalHeight
                    style: view.style
                    curve: view.measured.curve !== undefined ? view.measured.curve : []
                    rangeDb: view.measured.curveRange !== undefined ? view.measured.curveRange : 12
                }
                Caption { text: "BY REGION" }
                Column {
                    width: parent.width
                    Repeater {
                        model: {
                            const rows = [{ name: "", crest: "Crest dB", onsets: "Hits / s", width: "Side − mid dB" }]
                            const regions = view.measured.regions !== undefined ? view.measured.regions : []
                            for (let i = 0; i < regions.length; ++i)
                                rows.push(regions[i])
                            return rows
                        }
                        delegate: Row {
                            id: regionRow
                            required property var modelData
                            required property int index
                            readonly property real cell: (measureColumn.width - view.style.regionNameWidth) / 3
                            height: view.style.regionRowHeight
                            Text {
                                width: view.style.regionNameWidth
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: regionRow.modelData.name
                                color: view.style.textMain
                                font.pixelSize: view.style.typeBody
                            }
                            Repeater {
                                model: ["crest", "onsets", "width"]
                                delegate: Text {
                                    required property var modelData
                                    width: regionRow.cell
                                    height: regionRow.height
                                    verticalAlignment: Text.AlignVCenter
                                    horizontalAlignment: Text.AlignRight
                                    text: regionRow.modelData[modelData]
                                    color: regionRow.index === 0 ? view.style.textFaint : view.style.textMain
                                    font.family: regionRow.index === 0 ? view.style.sansFamily : view.style.monoFamily
                                    font.pixelSize: regionRow.index === 0 ? view.style.typeSmall : view.style.typeBody
                                }
                            }
                        }
                    }
                }
                Line {
                    width: parent.width
                    text: view.hasMeasure ? "Under 120 Hz the side sits " + (-Number(view.measured.lowSide)).toFixed(1)
                                            + " dB under the mid; the balance moves " + view.measured.spread
                                            + " dB through the song" : ""
                }
            }
        }
    }

    // ---- Proposal --------------------------------------------------------------------
    Rectangle {
        id: proposal
        objectName: "assistantProposal"
        anchors.right: parent.right
        anchors.rightMargin: view.style.viewMargin
        anchors.top: goals.top
        anchors.bottom: goals.bottom
        width: view.style.proposalWidth
        radius: view.style.cardRadius
        color: view.style.panel
        border.color: view.phase === "proposed" ? view.style.proposalEdge : view.style.line
        readonly property var devices: view.ready ? view.c.devices : []
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * view.style.cardPadding
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: proposal.devices.length === 0 && !view.working
            text: "The proposed chain appears here, each device with the measurement behind it"
            color: view.style.textFaint
            font.pixelSize: view.style.typeBody
        }
        Flickable {
            id: proposalScroll
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: actions.top
            anchors.margins: view.style.cardPadding
            contentHeight: proposalColumn.height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            visible: proposal.devices.length > 0
            Column {
                id: proposalColumn
                width: parent.width
                spacing: view.style.rowGap
                Caption {
                    text: view.phase === "accepted" || view.phase === "verified" ? "ACCEPTED" : "PROPOSED CHAIN"
                }
                Repeater {
                    model: view.ready ? view.c.nudges : []
                    delegate: ProposedRow {
                        required property var modelData
                        objectName: "assistantNudge:" + modelData.index
                        name: modelData.role
                        settings: (modelData.gain > 0 ? "+" : "") + modelData.gain.toFixed(1) + " dB in the rebalance"
                        reason: modelData.reason
                        kept: modelData.accepted
                        onToggled: on => view.c.setNudgeAccepted(modelData.index, on)
                    }
                }
                Repeater {
                    model: proposal.devices
                    delegate: ProposedRow {
                        required property var modelData
                        objectName: "assistantDevice:" + modelData.engine
                        name: modelData.name
                        settings: modelData.settings
                        reason: modelData.reason
                        kept: modelData.accepted
                        onToggled: on => view.c.setDeviceAccepted(modelData.index, on)
                    }
                }
                Caption { text: "LEFT OUT"; visible: notes.count > 0 }
                Repeater {
                    id: notes
                    model: view.ready ? view.c.notes : []
                    delegate: Line {
                        required property var modelData
                        width: proposalColumn.width
                        text: modelData
                        color: view.style.textFaint
                    }
                }
                Caption { text: "PREDICTED, MEASURED ON THE MIX" }
                Repeater {
                    model: view.ready ? view.c.prediction : []
                    delegate: Line {
                        required property var modelData
                        width: proposalColumn.width
                        text: modelData
                    }
                }
                Caption {
                    objectName: "assistantVerifiedCaption"
                    visible: view.ready && view.c.verification.length > 0
                    text: view.ready && view.c.verifiedOnTarget ? "THE MASTER AS IT NOW RENDERS: ON TARGET"
                                                                : "THE MASTER AS IT NOW RENDERS"
                    color: view.ready && view.c.verifiedOnTarget ? view.style.accent : view.style.amber
                }
                Repeater {
                    model: view.ready ? view.c.verification : []
                    delegate: Line {
                        required property var modelData
                        width: proposalColumn.width
                        text: modelData
                        color: view.style.textMain
                    }
                }
            }
        }

        // The A/B and the decision.
        Column {
            id: actions
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: view.style.cardPadding
            spacing: view.style.rowGap
            visible: view.phase === "proposed"
            Line {
                objectName: "assistantMatched"
                width: parent.width
                readonly property var info: view.ready ? view.c.audition : ({})
                text: info.currentLufs !== undefined
                      ? "A/B at matched loudness over the loudest " + Math.round(info.seconds) + " s: the current master "
                        + info.currentLufs + " LUFS (" + info.currentGain + " dB), the proposal " + info.proposedLufs
                        + " LUFS (" + info.proposedGain + " dB)"
                      : ""
            }
            Row {
                spacing: view.style.spaceMd
                HeaderButton {
                    objectName: "assistantPlay"
                    anchors.verticalCenter: undefined
                    label: view.ready && view.c.auditioning ? "Stop" : "Play A/B"
                    tip: "Loop the loudest stretch, level-matched, through the current master or the proposal"
                    Accessible.name: view.ready && view.c.auditioning ? "Stop the A/B" : "Play the A/B"
                    onClicked: view.c.auditioning ? view.c.stopAudition() : view.c.startAudition()
                }
                AssistantChips {
                    style: view.style
                    prefix: "assistantSide:"
                    model: [{ id: "current", name: "A · Current" }, { id: "proposal", name: "B · Proposal" }]
                    current: view.ready && view.c.hearingProposal ? "proposal" : "current"
                    tip: "Switch between the two at the same place; the louder is turned down to match"
                    onChosen: id => view.c.hearProposal(id === "proposal")
                }
            }
            Row {
                spacing: view.style.spaceMd
                HeaderButton {
                    objectName: "assistantAccept"
                    anchors.verticalCenter: undefined
                    accent: true
                    label: "Accept"
                    tip: "Put the kept devices on the master and move the kept roles: one undo step"
                    Accessible.name: "Accept the kept devices"
                    onClicked: view.c.accept()
                }
                HeaderButton {
                    objectName: "assistantReject"
                    anchors.verticalCenter: undefined
                    label: "Reject"
                    tip: "Drop the proposal: the project is as it was"
                    Accessible.name: "Reject the proposal"
                    onClicked: view.c.reject()
                }
            }
        }
    }

    // A proposed device or nudge: kept or left out, its settings and why.
    component ProposedRow: Rectangle {
        id: row
        property string name
        property string settings
        property string reason
        property bool kept: true
        signal toggled(bool on)
        width: proposalColumn.width
        height: rowColumn.height + 2 * view.style.spaceMd
        radius: view.style.radiusLg
        color: view.style.panelRaised
        border.color: row.kept ? view.style.lineStrong : view.style.line
        opacity: row.kept ? view.style.keptOpacity : view.style.droppedOpacity
        Rectangle {
            id: check
            x: view.style.spaceMd
            y: view.style.spaceMd
            width: view.style.checkSize
            height: view.style.checkSize
            radius: view.style.radiusSm
            color: row.kept ? view.style.accent : "transparent"
            border.color: row.kept ? view.style.accent : view.style.lineStrong
            activeFocusOnTab: view.phase === "proposed"
            Keys.onSpacePressed: row.toggled(!row.kept)
            Keys.onReturnPressed: row.toggled(!row.kept)
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Keep " + row.name
            FocusRing { targetRadius: view.style.radiusSm }
            Accessible.checked: row.kept
            MouseArea {
                anchors.fill: parent
                anchors.margins: -view.style.spaceXs
                enabled: view.phase === "proposed"
                cursorShape: Qt.PointingHandCursor
                onClicked: row.toggled(!row.kept)
                Tip { text: row.kept ? "Leave this one out" : "Keep this one" }
            }
        }
        Column {
            id: rowColumn
            x: check.x + check.width + view.style.spaceMd
            y: view.style.spaceMd
            width: row.width - x - view.style.spaceMd
            spacing: view.style.spaceXxs
            Text {
                text: row.name
                color: view.style.textMain
                font.pixelSize: view.style.typeLabel
                font.bold: true
            }
            Text {
                width: rowColumn.width
                text: row.settings
                wrapMode: Text.WordWrap
                color: view.style.accent
                font.family: view.style.monoFamily
                font.pixelSize: view.style.typeSmall
            }
            Text {
                width: rowColumn.width
                text: row.reason
                wrapMode: Text.WordWrap
                color: view.style.textDim
                font.pixelSize: view.style.typeBody
            }
        }
    }
}
