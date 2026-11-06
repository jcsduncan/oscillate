pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Warp editor for the selected audio clip: saved stretch mode and override,
// semitone + cent transpose, the active mode's settings (other modes keep
// theirs), Complex Pro formants, reviewed warp markers with the source's saved
// transient suggestions (apply or dismiss), auto-warp and tempo conform
// proposals for review, and an honest account of what playback is rendering
// now. Every change is one saved, undoable edit.
Item {
    id: editor
    required property var bridge
    required property var controller
    property color textMain: Theme.warpTextMain
    property color textDim: Theme.warpTextDim
    property color accent: Theme.warpAccent
    property color amber: Theme.warpAmber
    property color danger: Theme.warpDanger
    property color lineColor: Theme.warpLine
    readonly property var warp: controller ? controller.state : ({})
    readonly property var live: controller ? controller.playback : ({})
    readonly property bool editable: warp.available === true && warp.editable === true
    readonly property var modeNames: ["Beats", "Tones", "Texture", "Re-Pitch", "Complex", "Complex Pro"]
    readonly property var modeHints: [
        "Slices at transients, each slice unstretched",
        "Pitch-synchronous grains for monophonic material",
        "Granular for textures and ambiences",
        "Varispeed: pitch follows speed",
        "Phase vocoder for mixed material",
        "Complex plus formant preservation"
    ]
    implicitHeight: content.implicitHeight

    component Pill: Rectangle {
        id: pill
        required property string label
        property bool selected: false
        property bool enabledPill: true
        property string accessibleName: label
        signal activated()
        height: Theme.warpPillHeight
        width: Math.max(Theme.warpPillMinWidth, pillText.implicitWidth + Theme.spaceXl)
        radius: Theme.warpPillRadius
        color: selected ? editor.accent : Theme.warpControlFill
        border.color: activeFocus ? editor.textMain : editor.lineColor
        opacity: enabledPill ? 1 : Theme.warpUnavailableOpacity
        activeFocusOnTab: enabledPill
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName
        Tip { text: pill.accessibleName }
        Accessible.checked: selected
        Keys.onReturnPressed: if (enabledPill) activated()
        Keys.onSpacePressed: if (enabledPill) activated()
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            color: pill.selected ? Theme.warpPillSelectedText : editor.textMain
            font.pixelSize: Theme.typeCaption
            font.bold: pill.selected
        }
        MouseArea { anchors.fill: parent; enabled: pill.enabledPill; onClicked: pill.activated() }
    }

    Column {
        id: content
        width: editor.width
        spacing: Theme.spaceMd
        visible: editor.warp.available === true

        Item {
            width: parent.width
            height: Theme.warpHeaderHeight
            Text {
                text: "WARP"
                color: editor.textDim
                font.pixelSize: Theme.typeSmall
                font.bold: true
                font.letterSpacing: Theme.warpTitleLetterSpacing
            }
            Text {
                objectName: "warpRenderBadge"
                anchors.right: parent.right
                text: editor.warp.processed ? "PROCESSED" : "SOURCE"
                color: editor.warp.processed ? editor.accent : editor.textDim
                font.pixelSize: Theme.typeCaption
                font.bold: true
            }
        }
        Text {
            objectName: "warpRenderMessage"
            width: parent.width
            text: editor.warp.renderMessage || ""
            color: editor.textDim
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }
        Rectangle {
            objectName: "warpPlaybackStatus"
            width: parent.width
            height: playbackText.implicitHeight + Theme.spaceLg
            radius: Theme.warpStatusRadius
            color: Theme.warpStatusGround
            border.color: editor.live.state === "failed" ? editor.danger
                        : editor.live.state === "pending" ? editor.amber
                        : editor.live.state === "active" ? editor.accent : editor.lineColor
            Accessible.role: Accessible.StaticText
            Accessible.name: "Warp playback status"
            Accessible.description: playbackText.text
            Text {
                id: playbackText
                objectName: "warpPlaybackText"
                x: Theme.spaceMd; y: Theme.spaceSm
                width: parent.width - 2 * Theme.spaceMd
                text: (editor.live.state === "pending" ? "PENDING · " : editor.live.state === "failed" ? "NOT AUDIBLE · "
                       : editor.live.state === "active" ? "AUDIBLE · " : "") + (editor.live.text || "")
                color: editor.live.state === "failed" ? editor.danger
                     : editor.live.state === "pending" ? editor.amber : editor.textMain
                font.pixelSize: Theme.typeCaption
                wrapMode: Text.WordWrap
            }
        }

        // Mode.
        Text { text: "MODE"; color: editor.textDim; font.pixelSize: Theme.typeCaption; font.bold: true }
        Flow {
            objectName: "warpModeSelector"
            width: parent.width
            spacing: Theme.spaceXs
            Repeater {
                model: editor.modeNames
                delegate: Pill {
                    required property string modelData
                    required property int index
                    objectName: "warpMode" + modelData.replace(/[^A-Za-z]/g, "")
                    label: modelData
                    selected: editor.warp.modeIndex === index
                    enabledPill: editor.editable
                    accessibleName: "Warp mode " + modelData + ". " + editor.modeHints[index]
                    onActivated: editor.controller.set_mode(index)
                }
            }
        }
        Row {
            spacing: Theme.spaceSm
            Pill {
                objectName: "warpModeOverride"
                label: editor.warp.overridden ? "YOUR CHOICE" : "SUGGESTED"
                selected: editor.warp.overridden === true
                enabledPill: editor.editable
                accessibleName: editor.warp.overridden
                                ? "Mode is your choice; analysis will not change it. Activate to release"
                                : "Mode may be replaced by an analysis suggestion. Activate to keep it"
                onActivated: editor.controller.set_mode_overridden(!editor.warp.overridden)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: editor.modeHints[editor.warp.modeIndex] || ""
                color: editor.textDim
                font.pixelSize: Theme.typeCaption
            }
        }

        // Transpose.
        Text { text: "TRANSPOSE"; color: editor.textDim; font.pixelSize: Theme.typeCaption; font.bold: true }
        WarpStepper {
            objectName: "warpSemitones"
            width: parent.width
            label: "Semitones"
            value: editor.warp.semitones || 0
            minimum: -48; maximum: 48
            active: editor.warp.transposeActive === true
            inactiveNote: "Inactive in Re-Pitch"
            editable: editor.editable
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
            onCommitted: (value) => editor.controller.set_transpose(Math.round(value), editor.warp.cents)
        }
        WarpStepper {
            objectName: "warpCents"
            width: parent.width
            label: "Cents"
            value: editor.warp.cents || 0
            minimum: -50; maximum: 50
            active: editor.warp.transposeActive === true
            inactiveNote: "Inactive in Re-Pitch"
            editable: editor.editable
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
            onCommitted: (value) => editor.controller.set_transpose(editor.warp.semitones, Math.round(value))
        }

        // Settings of the active mode; other modes' settings are kept.
        Text {
            text: (editor.modeNames[editor.warp.modeIndex] || "").toUpperCase() + " SETTINGS"
            color: editor.textDim; font.pixelSize: Theme.typeCaption; font.bold: true
        }
        Column {
            width: parent.width
            spacing: Theme.spaceXs
            visible: editor.warp.modeIndex === 0
            Row {
                spacing: Theme.spaceXs
                Text { width: Theme.warpLabelWidth; text: "Gap fill"; color: editor.textMain; font.pixelSize: Theme.typeSmall; anchors.verticalCenter: parent.verticalCenter }
                Repeater {
                    model: ["Off", "Forward", "Back & forth"]
                    delegate: Pill {
                        required property string modelData
                        required property int index
                        objectName: "warpBeatsLoop" + index
                        label: modelData
                        selected: editor.warp.beatsLoop === index
                        enabledPill: editor.editable
                        accessibleName: "Beats gap fill " + modelData
                        onActivated: editor.controller.set_beats_loop(index)
                    }
                }
            }
            WarpStepper {
                width: parent.width
                label: "Envelope"
                value: editor.warp.beatsEnvelope || 0
                minimum: 0; maximum: 100; unit: "%"
                editable: editor.editable
                textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
                onCommitted: (value) => editor.controller.set_setting("beatsEnvelope", value)
            }
        }
        WarpStepper {
            width: parent.width
            visible: editor.warp.modeIndex === 1
            label: "Grain size"
            value: editor.warp.tonesGrain || 40
            minimum: 5; maximum: 200; unit: "ms"
            editable: editor.editable
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
            onCommitted: (value) => editor.controller.set_setting("tonesGrain", value)
        }
        Column {
            width: parent.width
            spacing: Theme.spaceXs
            visible: editor.warp.modeIndex === 2
            WarpStepper {
                width: parent.width
                label: "Grain size"
                value: editor.warp.textureGrain || 100
                minimum: 10; maximum: 500; unit: "ms"
                editable: editor.editable
                textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
                onCommitted: (value) => editor.controller.set_setting("textureGrain", value)
            }
            WarpStepper {
                width: parent.width
                label: "Flux"
                value: editor.warp.textureFlux || 0
                minimum: 0; maximum: 100; unit: "%"
                editable: editor.editable
                textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
                onCommitted: (value) => editor.controller.set_setting("textureFlux", value)
            }
        }
        Text {
            width: parent.width
            visible: editor.warp.modeIndex === 3 || editor.warp.modeIndex === 4
            text: editor.warp.modeIndex === 3
                  ? "Re-Pitch has no settings: pitch follows speed, so transpose and formants are inactive."
                  : "Complex has no extra settings."
            color: editor.textDim
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }
        Row {
            spacing: Theme.spaceSm
            Pill {
                objectName: "warpPreserveFormants"
                label: editor.warp.preserveFormants ? "FORMANTS KEPT" : "FORMANTS SHIFT"
                selected: editor.warp.preserveFormants === true
                enabledPill: editor.editable
                accessibleName: "Preserve formants " + (editor.warp.preserveFormants ? "on" : "off")
                                + (editor.warp.formantsActive ? "" : ". Inactive: applies in Complex Pro only")
                onActivated: editor.controller.set_preserve_formants(!editor.warp.preserveFormants)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: editor.warp.formantsActive ? "Applied by Complex Pro" : "Inactive: applies in Complex Pro only"
                color: editor.warp.formantsActive ? editor.textMain : editor.textDim
                font.pixelSize: Theme.typeCaption
                font.italic: !editor.warp.formantsActive
            }
        }
        WarpStepper {
            objectName: "warpFormantAmount"
            width: parent.width
            label: "Formant amount"
            value: editor.warp.formantAmount || 0
            minimum: 0; maximum: 100; unit: "%"
            active: editor.warp.formantsActive === true && editor.warp.preserveFormants === true
            inactiveNote: editor.warp.formantsActive ? "Formants shift (off)" : "Complex Pro only"
            editable: editor.editable
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
            onCommitted: (value) => editor.controller.set_setting("formantAmount", value)
        }
        WarpStepper {
            width: parent.width
            label: "Envelope detail"
            value: editor.warp.complexProEnvelope || 128
            minimum: 8; maximum: 256
            active: editor.warp.formantsActive === true
            inactiveNote: "Complex Pro only"
            editable: editor.editable
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; lineColor: editor.lineColor
            onCommitted: (value) => editor.controller.set_setting("complexProEnvelope", value)
        }

        // Markers.
        Item {
            width: parent.width
            height: Theme.warpHeaderHeight
            Text { text: "WARP MARKERS"; color: editor.textDim; font.pixelSize: Theme.typeCaption; font.bold: true }
            Text {
                anchors.right: parent.right
                text: (editor.warp.markers ? editor.warp.markers.length : 0) + " markers · "
                      + (editor.warp.musical ? "musical (ticks)" : "elapsed (samples)")
                color: editor.textDim
                font.pixelSize: Theme.typeCaption
            }
        }
        WarpMarkerLane {
            id: markerLane
            width: parent.width
            height: Theme.warpMarkerLaneHeight
            warpState: editor.warp
            controller: editor.controller
            editable: editor.editable
            snap: snapPill.selected
            textMain: editor.textMain; textDim: editor.textDim; accent: editor.accent; amber: editor.amber; lineColor: editor.lineColor
        }
        Flow {
            width: parent.width
            spacing: Theme.spaceXs
            Pill {
                id: snapPill
                objectName: "warpSnap"
                label: "SNAP"
                selected: true
                accessibleName: "Snap markers to beats " + (selected ? "on" : "off")
                onActivated: selected = !selected
            }
            Pill {
                objectName: "warpAddMarker"
                label: "ADD AT CURSOR"
                enabledPill: editor.editable
                onActivated: editor.controller.add_marker(markerLane.cursorFraction)
            }
            Pill {
                objectName: "warpDeleteMarker"
                label: "DELETE"
                enabledPill: editor.editable && markerLane.selectedMarker >= 0
                accessibleName: "Delete selected warp marker"
                onActivated: editor.controller.remove_marker(markerLane.selectedMarker)
            }
            Pill {
                objectName: "warpUndo"
                label: "UNDO"
                accessibleName: "Undo the last edit"
                onActivated: editor.bridge.undo()
            }
        }
        Flow {
            width: parent.width
            spacing: Theme.spaceXs
            Pill {
                objectName: "warpAnalyse"
                label: editor.warp.suggestionState === "running" ? "ANALYSING…"
                       : editor.warp.suggestions && editor.warp.suggestions.length > 0 ? "REANALYSE TRANSIENTS"
                       : "FIND TRANSIENTS"
                enabledPill: editor.editable && editor.warp.suggestionState !== "running"
                accessibleName: "Analyse the source's transients and save them as suggestions. Dismissed ones stay dismissed"
                onActivated: editor.controller.request_suggestions()
            }
            Pill {
                objectName: "warpApplySuggestions"
                label: "APPLY " + (editor.warp.suggestions ? editor.warp.suggestions.length : 0)
                enabledPill: editor.editable && editor.warp.suggestions && editor.warp.suggestions.length > 0
                accessibleName: "Apply the suggestions as warp markers, without moving audio"
                onActivated: editor.controller.apply_suggestions()
            }
            Pill {
                objectName: "warpDismissSuggestions"
                label: "DISMISS ALL"
                enabledPill: editor.editable && editor.warp.suggestions && editor.warp.suggestions.length > 0
                accessibleName: "Dismiss every suggestion shown. Reanalysis will not bring them back"
                onActivated: editor.controller.dismiss_suggestions()
            }
        }
        Text {
            objectName: "warpSuggestionStatus"
            width: parent.width
            text: editor.warp.suggestionMessage
                  || ((editor.warp.suggestions && editor.warp.suggestions.length > 0)
                      ? editor.warp.suggestions.length + " saved transient suggestions (hollow). Press D or right-click one to dismiss it."
                      : "No transient suggestions for this source. Find transients to analyse it.")
            color: editor.warp.suggestionState === "failed" || editor.warp.suggestionState === "stale" ? editor.amber : editor.textDim
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }
        Flow {
            width: parent.width
            spacing: Theme.spaceXs
            Pill {
                objectName: "warpAutoWarp"
                label: "AUTO-WARP"
                enabledPill: editor.editable && editor.warp.musical === true
                accessibleName: "Propose warp markers on the source's reviewed beats, aligned to the project grid"
                onActivated: editor.controller.propose_auto_warp()
            }
            Pill {
                objectName: "warpConform"
                label: "CONFORM TO PROJECT TEMPO"
                enabledPill: editor.editable && editor.warp.musical === true
                accessibleName: "Propose stretching the clip so its reviewed tempo plays at the project tempo"
                onActivated: editor.controller.propose_tempo_conform()
            }
            Pill {
                objectName: "warpApplyProposal"
                label: "APPLY PROPOSAL"
                enabledPill: editor.editable && editor.warp.proposal !== undefined
                accessibleName: "Apply the reviewed warp proposal as one edit"
                onActivated: editor.controller.apply_proposal()
            }
            Pill {
                objectName: "warpDiscardProposal"
                label: "DISCARD"
                enabledPill: editor.warp.proposal !== undefined
                accessibleName: "Discard the warp proposal"
                onActivated: editor.controller.discard_proposal()
            }
        }
        Text {
            objectName: "warpProposalStatus"
            width: parent.width
            text: editor.warp.proposal !== undefined ? editor.warp.proposal.summary
                  : editor.warp.proposalMessage
                  || (editor.warp.musical === true ? "Auto-warp and conform use this source's reviewed tempo and beats; your markers are never moved."
                                                    : "Switch the clip to musical time to auto-warp or conform it to the project tempo.")
            color: editor.warp.proposal !== undefined ? editor.accent : editor.textDim
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }
}
