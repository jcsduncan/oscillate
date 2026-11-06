pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import "shell"

// The piano roll's musical tools (ADR-0059), opened from TOOLS: the song's
// key with Scale lock and Fold, chord stamps, strum, humanise, quantise with
// strength and swing, and the groove pool. Every action is one undo step on
// the selected notes, or the whole clip when none are selected.
Rectangle {
    id: tools
    objectName: "pianoRollTools"
    required property var bridge
    required property var noteTools
    required property string clipId
    required property var selectedNoteIds
    required property int gridSteps
    required property bool foldToScale
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    // The chord the stamp draws, shared with the roll.
    property string chordShape: "Triad"
    property int chordInversion: 0
    property bool chordMode: false
    property int strumAmount: 4
    property real humanizeTiming: 0.35
    property real humanizeVelocity: 0.4
    property real quantizeStrength: 1.0
    property real quantizeSwing: 0.5
    property bool quantizeEnds: false
    signal foldToggled()
    signal closeRequested()
    // The panel unfolds under TOOLS.
    onVisibleChanged: if (visible && Motion.enabled) unfold.restart()
    transformOrigin: Item.TopRight
    ParallelAnimation {
        id: unfold
        NumberAnimation { target: tools; property: "opacity"; from: 0; to: 1
                          duration: Motion.fast }
        NumberAnimation { target: tools; property: "scale"; from: Theme.pianoToolsUnfoldScale; to: 1
                          duration: Motion.normal; easing.type: Easing.OutCubic }
    }
    // Import… and Export… ask for a file path in the groove pane.
    property string groovePathMode: ""
    // Layout: three panes, left to right (key and chords, feel, groove),
    // split by dividers; each control sits on its pane's rows.
    readonly property int paneTop: 10
    readonly property int paneRowY: 16
    readonly property int keyPaneX: 12
    readonly property int keyPaneWidth: 248
    readonly property int feelDividerX: 268
    readonly property int feelPaneX: 280
    readonly property int feelPaneWidth: 236
    readonly property int grooveDividerX: 524
    readonly property int groovePaneX: 536
    readonly property int groovePaneWidth: 230
    readonly property int tonicsPerRow: 6
    readonly property int tonicColumnStride: 41
    readonly property int tonicRowStride: 23
    readonly property int tonicWidth: 38
    readonly property int keyModeRowY: 64
    readonly property int keyModeWidth: 60
    readonly property int minorX: 64
    readonly property int lockX: 132
    readonly property int foldX: 190
    readonly property int keyToggleWidth: 54
    readonly property int chordHeadingY: 96
    readonly property int chordRowY: 112
    readonly property int chordStride: 49
    readonly property int chordWidth: 46
    readonly property int inversionCount: 4
    readonly property int inversionLabelY: 141
    readonly property int inversionRowY: 137
    readonly property int inversionX: 74
    readonly property int inversionStride: 32
    readonly property int inversionWidth: 28
    readonly property int chordOffX: 204
    readonly property int strumHeadingY: 168
    readonly property int strumRowY: 182
    readonly property int strumSlideWidth: 150
    readonly property int strumLabelWidth: 40
    readonly property int strumButtonY: 181
    readonly property int strumUpX: 156
    readonly property int strumDownX: 204
    readonly property int strumButtonWidth: 44
    readonly property int humaniseVelocityY: 40
    readonly property int humaniseActionY: 66
    readonly property int quantiseHeadingY: 100
    readonly property int quantiseStrengthY: 116
    readonly property int quantiseSwingY: 140
    readonly property int quantiseEndsY: 167
    readonly property int quantiseEndsWidth: 64
    readonly property int quantiseActionX: 72
    readonly property int quantiseActionY: 165
    readonly property int groovePathHeight: 40
    readonly property int grooveListHeight: 92
    readonly property int grooveQuantiseY: 112
    readonly property int grooveTimingY: 134
    readonly property int grooveVelocityY: 156
    readonly property int grooveApplyY: 182

    readonly property string scopeText: selectedNoteIds.length > 0
        ? selectedNoteIds.length + " SELECTED" : "WHOLE CLIP"
    readonly property var keyNames: Contracts.pitch_class_names(Contracts.key)
    readonly property string keyText: bridge.projectKey
    readonly property bool keyKnown: bridge.projectScale.length > 0
    readonly property int keyTonic: keyKnown ? Number(bridge.projectScale[0]) : -1
    // A minor scale's third is three semitones up.
    readonly property bool keyMinor: keyKnown
        && (Number(bridge.projectScale[2]) - keyTonic + 12) % 12 === 3

    width: Theme.pianoToolsWidth
    height: Theme.pianoToolsHeight
    radius: Theme.radiusXl
    color: Theme.pianoToolsGround
    border.color: lineColor
    activeFocusOnTab: visible
    Accessible.role: Accessible.Dialog
    Accessible.name: "Piano roll tools: key, chords, strum, humanise, quantise and groove. Acting on "
                     + scopeText.toLowerCase()
    Keys.onEscapePressed: closeRequested()
    // Clicks inside never reach the grid under it.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    component Heading: Text {
        color: tools.dimTextColor
        font.pixelSize: Theme.typeCaption; font.bold: true; font.letterSpacing: Theme.capsLetterSpacing
    }
    component Chip: Rectangle {
        id: chip
        property string label
        property bool chosen: false
        property string help: label
        property color tint: tools.accentColor
        signal activated()
        width: Theme.pianoToolsChipWidth; height: Theme.pianoToolsChipHeight; radius: Theme.radiusMd
        color: chosen ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? tools.accentColor : tools.lineColor
        activeFocusOnTab: tools.visible
        Accessible.role: Accessible.Button
        Accessible.name: help
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text { anchors.centerIn: parent; text: chip.label
            color: chip.chosen ? chip.tint : tools.mainTextColor
            font.pixelSize: Theme.typeCaption; font.bold: true }
        Tip { text: chip.help }
        MouseArea { anchors.fill: parent; onClicked: chip.activated() }
    }
    component Slide: Item {
        id: slide
        property string label
        property real from: 0
        property real to: 1
        property real value: 0
        property string valueText: Math.round(100 * (value - from) / (to - from)) + "%"
        property real labelWidth: Theme.pianoToolsSlideLabelWidth
        signal moved(real value)
        // Arrow keys move a twentieth of the range.
        readonly property int keySteps: 20
        width: Theme.pianoToolsSlideWidth; height: Theme.pianoToolsSlideHeight
        Accessible.role: Accessible.Slider
        Accessible.name: label + " " + valueText
        activeFocusOnTab: tools.visible
        Keys.onLeftPressed: moved(Math.max(from, value - (to - from) / keySteps))
        Keys.onRightPressed: moved(Math.min(to, value + (to - from) / keySteps))
        Text { y: Theme.pianoToolsSlideTextY; text: slide.label; color: tools.mainTextColor; font.pixelSize: Theme.typeCaption }
        Rectangle {
            id: track
            x: slide.labelWidth; y: Theme.pianoToolsSlideTrackY; width: slide.width - slide.labelWidth - Theme.pianoToolsSlideValueReserve; height: Theme.pianoToolsSlideTrackHeight; radius: Theme.radiusXs
            color: Theme.pianoToolsSlideTrack
            Rectangle { width: parent.width * (slide.value - slide.from) / (slide.to - slide.from)
                height: parent.height; radius: Theme.radiusXs; color: tools.accentColor; opacity: Theme.pianoToolsSlideFillOpacity }
            Rectangle { x: parent.width * (slide.value - slide.from) / (slide.to - slide.from) - width / 2
                y: (parent.height - height) / 2; width: Theme.pianoToolsSlideThumbSize; height: Theme.pianoToolsSlideThumbSize; radius: width / 2
                color: slide.activeFocus ? tools.amberColor : tools.mainTextColor }
        }
        Text { x: slide.width - Theme.pianoToolsSlideValueWidth; y: Theme.pianoToolsSlideTextY; width: Theme.pianoToolsSlideValueWidth; horizontalAlignment: Text.AlignRight
            text: slide.valueText; color: tools.dimTextColor; font.pixelSize: Theme.typeCaption }
        Tip { text: slide.label + " " + slide.valueText + " — drag, or arrow keys" }
        MouseArea {
            x: track.x - Theme.spaceSm; width: track.width + 2 * Theme.spaceSm; height: slide.height
            preventStealing: true
            function set(mouseX) {
                const t = Math.max(0, Math.min(1, (mouseX - Theme.spaceSm) / track.width))
                slide.moved(slide.from + t * (slide.to - slide.from))
            }
            onPressed: mouse => set(mouse.x)
            onPositionChanged: mouse => { if (pressed) set(mouse.x) }
        }
    }
    component Action: Rectangle {
        id: action
        property string label
        property string help: label
        signal activated()
        width: Theme.pianoToolsActionWidth; height: Theme.pianoToolsActionHeight; radius: Theme.pianoToolsActionRadius
        color: hover.containsMouse ? Theme.menuHover : Theme.noteActionFill
        border.color: activeFocus ? tools.accentColor : tools.lineColor
        activeFocusOnTab: tools.visible
        Accessible.role: Accessible.Button
        Accessible.name: help
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text { anchors.centerIn: parent; text: action.label; color: tools.accentColor
            font.pixelSize: Theme.typeCaption; font.bold: true }
        Tip { text: action.help }
        MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; onClicked: action.activated() }
    }

    // --- Key and chords ------------------------------------------------------
    Item {
        x: tools.keyPaneX; y: tools.paneTop; width: tools.keyPaneWidth; height: parent.height - 2 * tools.paneTop
        Heading { text: "KEY · " + (tools.keyKnown ? tools.keyText.toUpperCase() : "NOT SET") }
        Repeater {
            model: 12
            delegate: Chip {
                required property int index
                objectName: "pianoToolsTonic" + index
                x: (index % tools.tonicsPerRow) * tools.tonicColumnStride; y: tools.paneRowY + Math.floor(index / tools.tonicsPerRow) * tools.tonicRowStride
                width: tools.tonicWidth
                label: tools.keyNames[index]
                chosen: tools.keyTonic === index
                help: "Set the song's key to " + tools.keyNames[index]
                      + (tools.keyMinor ? " minor" : " major")
                onActivated: tools.bridge.set_project_key(index, tools.keyMinor ? "minor" : "major")
            }
        }
        Chip { objectName: "pianoToolsMajor"; x: 0; y: tools.keyModeRowY; width: tools.keyModeWidth; label: "MAJOR"
            chosen: tools.keyKnown && !tools.keyMinor
            help: "Major key"
            onActivated: tools.bridge.set_project_key(Math.max(0, tools.keyTonic), "major") }
        Chip { objectName: "pianoToolsMinor"; x: tools.minorX; y: tools.keyModeRowY; width: tools.keyModeWidth; label: "MINOR"
            chosen: tools.keyKnown && tools.keyMinor
            help: "Minor key"
            onActivated: tools.bridge.set_project_key(Math.max(0, tools.keyTonic), "minor") }
        Chip { objectName: "pianoToolsScaleLock"; x: tools.lockX; y: tools.keyModeRowY; width: tools.keyToggleWidth; label: "LOCK"
            chosen: tools.noteTools.scaleLock
            help: tools.keyKnown ? "Scale lock: drawn and moved notes stay in " + tools.keyText
                                 : "Set a key to lock notes to its scale"
            opacity: tools.keyKnown ? 1 : Theme.pianoRollUnavailableOpacity
            onActivated: if (tools.keyKnown) tools.noteTools.scaleLock = !tools.noteTools.scaleLock }
        Chip { objectName: "pianoToolsFold"; x: tools.foldX; y: tools.keyModeRowY; width: tools.keyToggleWidth; label: "FOLD"
            chosen: tools.foldToScale
            help: "Fold: show only in-key rows and rows with notes"
            opacity: tools.keyKnown ? 1 : Theme.pianoRollUnavailableOpacity
            onActivated: if (tools.keyKnown) tools.foldToggled() }

        Heading { y: tools.chordHeadingY; text: "CHORD STAMP" + (tools.chordMode ? " · ON: CLICK THE GRID" : "") }
        Repeater {
            model: tools.noteTools.chordShapes
            delegate: Chip {
                required property string modelData
                required property int index
                objectName: "pianoToolsChord" + modelData
                x: index * tools.chordStride; y: tools.chordRowY; width: tools.chordWidth
                label: modelData.toUpperCase()
                chosen: tools.chordMode && tools.chordShape === modelData
                help: "Draw " + modelData + " chords with a click"
                      + (tools.keyKnown ? ", in " + tools.keyText : ", major")
                onActivated: {
                    tools.chordShape = modelData
                    tools.chordMode = true
                }
            }
        }
        Text { y: tools.inversionLabelY; text: "INVERSION"; color: tools.mainTextColor; font.pixelSize: Theme.typeCaption }
        Repeater {
            model: tools.inversionCount
            delegate: Chip {
                required property int index
                x: tools.inversionX + index * tools.inversionStride; y: tools.inversionRowY; width: tools.inversionWidth
                label: index === 0 ? "ROOT" : String(index)
                chosen: tools.chordInversion === index
                help: index === 0 ? "Root position" : "Inversion " + index
                onActivated: tools.chordInversion = index
            }
        }
        Chip { x: tools.chordOffX; y: tools.inversionRowY; width: Theme.pianoToolsChipWidth; label: "OFF"; help: "Stop drawing chords"
            visible: tools.chordMode; onActivated: tools.chordMode = false }

        Heading { y: tools.strumHeadingY; text: "STRUM" }
        Slide { y: tools.strumRowY; width: tools.strumSlideWidth; labelWidth: tools.strumLabelWidth; label: "Spread"; from: 1; to: 16; value: tools.strumAmount
            valueText: tools.strumAmount + "/128"
            onMoved: value => tools.strumAmount = Math.round(value) }
        Action { objectName: "pianoToolsStrumUp"; x: tools.strumUpX; y: tools.strumButtonY; width: tools.strumButtonWidth; label: "↑"
            help: "Strum up: lowest note first, " + tools.strumAmount + " 128th notes apart. One undo step."
            onActivated: tools.noteTools.strum(tools.clipId, tools.selectedNoteIds, tools.strumAmount, true) }
        Action { objectName: "pianoToolsStrumDown"; x: tools.strumDownX; y: tools.strumButtonY; width: tools.strumButtonWidth; label: "↓"
            help: "Strum down: highest note first. One undo step."
            onActivated: tools.noteTools.strum(tools.clipId, tools.selectedNoteIds, tools.strumAmount, false) }
    }

    Rectangle { x: tools.feelDividerX; y: tools.paneTop; width: Theme.hairline; height: parent.height - 2 * tools.paneTop; color: tools.lineColor }

    // --- Feel: humanise and quantise ----------------------------------------
    Item {
        x: tools.feelPaneX; y: tools.paneTop; width: tools.feelPaneWidth; height: parent.height - 2 * tools.paneTop
        Heading { text: "HUMANISE · " + tools.scopeText }
        Slide { y: tools.paneRowY; label: "Timing"; value: tools.humanizeTiming
            onMoved: value => tools.humanizeTiming = value }
        Slide { y: tools.humaniseVelocityY; label: "Velocity"; value: tools.humanizeVelocity
            onMoved: value => tools.humanizeVelocity = value }
        Action { objectName: "pianoToolsHumanise"; y: tools.humaniseActionY; label: "HUMANISE"
            help: "Nudge timing and velocity a little, one undo step; click again for another take"
            onActivated: tools.noteTools.humanize(tools.clipId, tools.selectedNoteIds,
                                                  tools.humanizeTiming, tools.humanizeVelocity) }

        Heading { y: tools.quantiseHeadingY; text: "QUANTISE · 1/" + tools.gridSteps }
        Slide { y: tools.quantiseStrengthY; label: "Strength"; value: tools.quantizeStrength
            onMoved: value => tools.quantizeStrength = value }
        Slide { y: tools.quantiseSwingY; label: "Swing"; from: 0.5; to: 0.75; value: tools.quantizeSwing
            valueText: Math.round(tools.quantizeSwing * 100) + "%"
            onMoved: value => tools.quantizeSwing = value }
        Chip { y: tools.quantiseEndsY; width: tools.quantiseEndsWidth; label: "ENDS"; chosen: tools.quantizeEnds
            help: "Also quantise note ends"
            onActivated: tools.quantizeEnds = !tools.quantizeEnds }
        Action { objectName: "pianoToolsQuantise"; x: tools.quantiseActionX; y: tools.quantiseActionY; label: "QUANTISE"
            help: "Quantise to the 1/" + tools.gridSteps + " grid at "
                  + Math.round(tools.quantizeStrength * 100) + "% strength, "
                  + Math.round(tools.quantizeSwing * 100) + "% swing. One undo step."
            onActivated: tools.noteTools.quantize(tools.clipId, tools.selectedNoteIds, tools.gridSteps,
                                                  tools.quantizeStrength, tools.quantizeSwing,
                                                  tools.quantizeEnds) }
    }

    Rectangle { x: tools.grooveDividerX; y: tools.paneTop; width: Theme.hairline; height: parent.height - 2 * tools.paneTop; color: tools.lineColor }

    // --- Groove pool -------------------------------------------------------------
    Item {
        id: groovePane
        x: tools.groovePaneX; y: tools.paneTop; width: tools.groovePaneWidth; height: parent.height - 2 * tools.paneTop
        Heading { text: "GROOVE POOL" }
        Row {
            anchors.right: closeText.left; anchors.rightMargin: Theme.spaceMd
            spacing: Theme.spaceSm
            Repeater {
                model: [{label: "RECORD…", mode: "record",
                         help: "Take a groove from a record or an audio clip"},
                        {label: "IMPORT…", mode: "import", help: "Add a groove file to the pool"},
                        {label: "EXPORT…", mode: "export",
                         help: "Save the chosen groove to a file you can share"}]
                delegate: Text {
                    id: fileLink
                    required property var modelData
                    objectName: "pianoToolsGroove" + (modelData.mode === "import" ? "Import"
                                                      : modelData.mode === "record" ? "Record" : "Export")
                    text: modelData.label
                    color: tools.groovePathMode === modelData.mode ? tools.accentColor : tools.dimTextColor
                    font.pixelSize: Theme.typeCaption; font.bold: true; font.letterSpacing: Theme.pianoToolsLinkLetterSpacing
                    activeFocusOnTab: tools.visible
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.help
                    function open() {
                        if (modelData.mode === "record") {
                            tools.bridge.grooveCapture.open = true
                            return
                        }
                        tools.groovePathMode = modelData.mode
                        groovePath.text = ""
                        groovePath.forceActiveFocus()
                    }
                    Keys.onReturnPressed: open()
                    Keys.onSpacePressed: open()
                    Tip { text: fileLink.modelData.help }
                    MouseArea { anchors.fill: parent; anchors.margins: -Theme.pianoToolsLinkHitOutset; onClicked: fileLink.open() }
                }
            }
        }
        Text { id: closeText; anchors.right: parent.right; text: "✕"; color: tools.dimTextColor; font.pixelSize: Theme.typeBody
            Tip { text: "Close the tools — Esc" }
            MouseArea { anchors.fill: parent; anchors.margins: -Theme.spaceXs; onClicked: tools.closeRequested() } }
        // The file path for Import… or Export…, over the top of the list.
        Rectangle {
            objectName: "pianoToolsGroovePath"
            visible: tools.groovePathMode !== ""
            z: 2
            y: tools.paneRowY; width: parent.width; height: tools.groovePathHeight; radius: Theme.radiusMd
            color: Theme.pianoToolsPathGround
            border.color: tools.accentColor
            Text {
                x: Theme.spaceSm; y: Theme.spaceXs
                text: tools.groovePathMode === "import" ? "IMPORT A GROOVE FILE"
                    : "EXPORT " + tools.noteTools.currentGroove.toUpperCase() + " TO"
                color: tools.dimTextColor; font.pixelSize: Theme.typeMicro; font.bold: true; font.letterSpacing: Theme.pianoToolsPathLetterSpacing
            }
            TextInput {
                id: groovePath
                x: Theme.spaceSm; y: Theme.pianoToolsPathInputY; width: parent.width - Theme.spaceLg
                color: tools.mainTextColor; font.pixelSize: Theme.typeSmall
                clip: true
                Accessible.role: Accessible.EditableText
                Accessible.name: "Groove file path; Enter confirms, Escape cancels"
                Text {
                    visible: groovePath.text.length === 0
                    text: tools.groovePathMode === "import" ? "/path/to/groove.json"
                                                            : "/path/to/" + tools.noteTools.currentGroove + ".json"
                    color: tools.dimTextColor; font.pixelSize: Theme.typeSmall
                }
                Keys.onReturnPressed: {
                    const path = text.trim()
                    if (path.length > 0) {
                        if (tools.groovePathMode === "import")
                            tools.noteTools.import_groove(path)
                        else
                            tools.noteTools.export_groove(tools.noteTools.currentGroove, path)
                    }
                    tools.groovePathMode = ""
                }
                Keys.onEscapePressed: tools.groovePathMode = ""
            }
        }
        ListView {
            id: grooveList
            objectName: "pianoToolsGrooves"
            y: tools.paneRowY; width: parent.width; height: tools.grooveListHeight
            clip: true
            model: tools.noteTools.grooves
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                id: grooveRow
                required property var modelData
                required property int index
                readonly property bool chosen: tools.noteTools.currentGroove === modelData.name
                width: grooveList.width; height: Theme.pianoToolsGrooveRowHeight; radius: Theme.radiusSm
                color: chosen ? Theme.toggleOn : (rowHover.containsMouse ? Theme.pianoToolsGrooveRowHover : "transparent")
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name + (modelData.user ? ", your groove" : "")
                Text { x: Theme.spaceXs; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - Theme.pianoToolsGrooveTextReserve; elide: Text.ElideRight
                    text: grooveRow.modelData.name
                    color: grooveRow.chosen ? tools.accentColor : tools.mainTextColor
                    font.pixelSize: Theme.typeCaption; font.bold: grooveRow.chosen }
                // The groove's feel: each step's push or drag, height its accent.
                Row {
                    id: feelRow
                    x: parent.width - Theme.pianoToolsGrooveFeelRight; anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.hairline
                    // At most feelSteps bars; an accent of maxAccent fills the height,
                    // and a step pushed or dragged past timingDeadband is coloured.
                    readonly property int feelSteps: 16
                    readonly property real maxAccent: 1.5
                    readonly property real timingDeadband: 0.01
                    Repeater {
                        model: Math.min(feelRow.feelSteps, grooveRow.modelData.timings.length)
                        delegate: Rectangle {
                            required property int index
                            width: Math.max(Theme.pianoToolsGrooveFeelMinBarWidth, Theme.pianoToolsGrooveFeelWidth / Math.min(feelRow.feelSteps, grooveRow.modelData.timings.length) - Theme.hairline)
                            height: Theme.pianoToolsGrooveFeelBase + Theme.pianoToolsGrooveFeelRange * Math.min(feelRow.maxAccent, Number(grooveRow.modelData.velocities[index])) / feelRow.maxAccent
                            anchors.bottom: parent.bottom
                            color: Number(grooveRow.modelData.timings[index]) > feelRow.timingDeadband ? tools.amberColor
                                 : Number(grooveRow.modelData.timings[index]) < -feelRow.timingDeadband ? Theme.pianoToolsGrooveEarly
                                 : tools.dimTextColor
                        }
                    }
                }
                Tip { text: grooveRow.modelData.name + (grooveRow.modelData.user ? " (yours)" : "")
                            + " — click chooses, double-click applies"
                            + (grooveRow.modelData.user ? ", right-click deletes" : "") }
                MouseArea { id: rowHover; anchors.fill: parent; hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton && grooveRow.modelData.user)
                            tools.noteTools.delete_groove(grooveRow.modelData.name)
                        else
                            tools.noteTools.currentGroove = grooveRow.modelData.name
                    }
                    onDoubleClicked: tools.noteTools.apply_current_groove(tools.clipId, tools.selectedNoteIds) }
            }
        }
        Slide { y: tools.grooveQuantiseY; width: tools.groovePaneWidth; label: "Quantise"; value: tools.noteTools.grooveQuantize
            onMoved: value => tools.noteTools.grooveQuantize = value }
        Slide { y: tools.grooveTimingY; width: tools.groovePaneWidth; label: "Timing"; value: tools.noteTools.grooveTiming
            onMoved: value => tools.noteTools.grooveTiming = value }
        Slide { y: tools.grooveVelocityY; width: tools.groovePaneWidth; label: "Velocity"; value: tools.noteTools.grooveVelocity
            onMoved: value => tools.noteTools.grooveVelocity = value }
        Action { objectName: "pianoToolsApplyGroove"; y: tools.grooveApplyY; width: tools.groovePaneWidth
            label: "APPLY " + tools.noteTools.currentGroove.toUpperCase()
            help: "Apply " + tools.noteTools.currentGroove + " to " + tools.scopeText.toLowerCase()
                  + ". One undo step. RECORD… takes a groove from a record or an audio clip."
            onActivated: tools.noteTools.apply_current_groove(tools.clipId, tools.selectedNoteIds) }
    }
}
