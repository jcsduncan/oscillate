pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The export dialog's contents: where the master goes (name, folder),
// its format and rate, the export itself with its progress and result,
// and under "More exports" the WAV stems and JACK real-time capture.
// Every setting lives on the bridge; this panel only shows and edits it.
Item {
    id: exportPanel
    required property var bridge
    required property color canvas
    required property color panelRaised
    required property color line
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber
    property bool exportFormatMenuOpen: false
    property bool exportRateMenuOpen: false
    property bool stemSourceMenuOpen: false
    property bool moreOpen: false
    property int selectedStemIndex: 0
    signal closeRequested()
    readonly property int margin: Theme.space2xl
    readonly property int gap: Theme.spaceMd
    // The progress sweep spans this fraction of the track (1 / parts).
    readonly property int progressSweepParts: 3
    readonly property bool twoInstrumentSources: bridge.exports.exportsSelectedSourceStems
    readonly property var stemSourceTracks: twoInstrumentSources ? bridge.tracks.filter(track => track.vst3SourceReady || track.nativeSourceReady || track.kind === "audio") : []
    readonly property var selectedStem: stemSourceTracks.length ? stemSourceTracks[Math.min(selectedStemIndex, stemSourceTracks.length - 1)] : null
    readonly property var exportFormatLabels: bridge.exports.exportFormatLabels
    readonly property var exportRateLabels: bridge.exports.exportRateLabels
    readonly property bool masterReady: bridge.exports.exportEnabled && bridge.exports.masterExportRateValid
    readonly property bool saved: !bridge.exports.exportBusy && bridge.exports.exportSavedPath.length > 0
    implicitHeight: (moreOpen ? moreSection.y + moreSection.height : footer.y + footer.height) + margin

    // Name and folder edits apply when focus leaves the field; an action
    // applies any edit still in progress first.
    function commitTarget() {
        bridge.exports.set_export_file_name(nameInput.text);
        bridge.exports.set_export_folder(folderInput.text);
    }
    function exportMaster() {
        commitTarget();
        bridge.exports.export_master();
    }
    function closeMenus() {
        exportFormatMenuOpen = false;
        exportRateMenuOpen = false;
        stemSourceMenuOpen = false;
    }
    function selectMasterFormat(index) {
        bridge.exports.exportFormatIndex = index;
        closeMenus();
    }
    function selectExportRate(index) {
        bridge.exports.exportRateIndex = index;
        closeMenus();
    }
    function selectStem(index) {
        selectedStemIndex = index;
        stemSourceMenuOpen = false;
    }
    function exportSelectedStem() {
        if (!selectedStem)
            return;
        commitTarget();
        bridge.exports.export_instrument_source_stem(selectedStem.id);
    }

    component Caption: Text {
        color: exportPanel.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.trackingXl
    }

    Caption {
        id: nameCaption
        x: exportPanel.margin
        y: 0
        text: "FILE NAME"
    }
    Rectangle {
        id: nameBox
        x: exportPanel.margin
        y: nameCaption.y + nameCaption.height + Theme.spaceSm
        width: exportPanel.width - 2 * exportPanel.margin
        height: Theme.exportFieldHeight
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: nameInput.activeFocus ? exportPanel.accent : exportPanel.line
        TextInput {
            id: nameInput
            objectName: "exportFileName"
            x: Theme.exportFieldPadding
            width: parent.width - extensionLabel.implicitWidth - Theme.exportNameRightPad
            anchors.verticalCenter: parent.verticalCenter
            text: exportPanel.bridge.exports.exportFileName
            color: exportPanel.textMain
            selectionColor: exportPanel.accent
            selectedTextColor: exportPanel.canvas
            font.pixelSize: Theme.typeLabel
            clip: true
            selectByMouse: true
            enabled: !exportPanel.bridge.exports.exportBusy
            Accessible.name: "Export file name"
            onEditingFinished: exportPanel.bridge.exports.set_export_file_name(text)
        }
        Text {
            id: extensionLabel
            anchors.right: parent.right
            anchors.rightMargin: Theme.exportFieldPadding
            anchors.verticalCenter: parent.verticalCenter
            text: exportPanel.bridge.exports.exportExtension
            color: exportPanel.textDim
            font.pixelSize: Theme.typeLabel
        }
    }
    Caption {
        id: folderCaption
        x: exportPanel.margin
        y: nameBox.y + nameBox.height + Theme.spaceLg
        text: "FOLDER"
    }
    Rectangle {
        id: folderBox
        x: exportPanel.margin
        y: folderCaption.y + folderCaption.height + Theme.spaceSm
        width: chooseButton.x - x - exportPanel.gap
        height: Theme.exportFieldHeight
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: folderInput.activeFocus ? exportPanel.accent : exportPanel.line
        TextInput {
            id: folderInput
            objectName: "exportFolder"
            anchors.fill: parent
            anchors.margins: Theme.exportFieldPadding
            verticalAlignment: TextInput.AlignVCenter
            text: exportPanel.bridge.exports.exportFolder
            color: exportPanel.textMain
            selectionColor: exportPanel.accent
            selectedTextColor: exportPanel.canvas
            font.pixelSize: Theme.typeBody
            clip: true
            selectByMouse: true
            enabled: !exportPanel.bridge.exports.exportBusy
            Accessible.name: "Export folder"
            onEditingFinished: exportPanel.bridge.exports.set_export_folder(text)
        }
    }
    Rectangle {
        id: chooseButton
        objectName: "exportChooseButton"
        x: exportPanel.width - exportPanel.margin - width
        y: folderBox.y
        width: Theme.exportSecondaryButtonWidth
        height: Theme.exportFieldHeight
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: activeFocus ? exportPanel.accent : exportPanel.line
        opacity: exportPanel.bridge.exports.exportEnabled ? 1 : Theme.exportDisabledOpacity
        activeFocusOnTab: exportPanel.bridge.exports.exportEnabled
        Accessible.role: Accessible.Button
        Accessible.name: "Choose the export file"
        Keys.onReturnPressed: exportPanel.bridge.exports.choose_export_target()
        Keys.onSpacePressed: exportPanel.bridge.exports.choose_export_target()
        Text {
            anchors.centerIn: parent
            text: "Choose…"
            color: exportPanel.textMain
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            Tip { text: "Pick the file name and folder" }
            anchors.fill: parent
            enabled: exportPanel.bridge.exports.exportEnabled
            onClicked: exportPanel.bridge.exports.choose_export_target()
        }
    }
    Text {
        id: replaceHint
        objectName: "exportReplaceHint"
        x: exportPanel.margin
        y: folderBox.y + folderBox.height + Theme.spaceSm
        width: exportPanel.width - 2 * exportPanel.margin
        // One line's height from the font: an eliding Text's implicitHeight
        // depends on its height, which loops.
        height: visible ? Math.ceil(hintMetrics.height) : 0
        visible: exportPanel.bridge.exports.exportReplacesFile && !exportPanel.bridge.exports.exportBusy
        text: "A file with this name is already there; exporting replaces it."
        color: exportPanel.amber
        font.pixelSize: Theme.typeSmall
        elide: Text.ElideRight
        FontMetrics { id: hintMetrics; font: replaceHint.font }
    }

    Caption {
        id: formatCaption
        x: exportPanel.margin
        y: replaceHint.y + replaceHint.height + Theme.spaceLg
        text: "FORMAT"
    }
    Caption {
        x: rateButton.x
        y: formatCaption.y
        text: "SAMPLE RATE"
    }
    Rectangle {
        id: formatButton
        objectName: "masterFormatButton"
        x: exportPanel.margin
        y: formatCaption.y + formatCaption.height + Theme.spaceSm
        width: (exportPanel.width - 2 * exportPanel.margin - exportPanel.gap) / 2
        height: Theme.exportFieldHeight
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: activeFocus || exportPanel.exportFormatMenuOpen ? exportPanel.accent : exportPanel.line
        activeFocusOnTab: exportPanel.bridge.exports.exportEnabled
        Accessible.role: Accessible.Button
        Accessible.name: "Format: " + exportPanel.exportFormatLabels[exportPanel.bridge.exports.exportFormatIndex]
        Keys.onReturnPressed: exportPanel.exportFormatMenuOpen = !exportPanel.exportFormatMenuOpen
        Keys.onSpacePressed: exportPanel.exportFormatMenuOpen = !exportPanel.exportFormatMenuOpen
        Text {
            x: Theme.exportFieldPadding
            anchors.verticalCenter: parent.verticalCenter
            text: exportPanel.exportFormatLabels[exportPanel.bridge.exports.exportFormatIndex]
            color: exportPanel.textMain
            font.pixelSize: Theme.typeLabel
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: Theme.exportFieldPadding
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            color: exportPanel.textDim
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            anchors.fill: parent
            enabled: exportPanel.bridge.exports.exportEnabled
            onClicked: {
                exportPanel.exportRateMenuOpen = false;
                exportPanel.exportFormatMenuOpen = !exportPanel.exportFormatMenuOpen;
            }
        }
    }
    Rectangle {
        id: rateButton
        objectName: "exportRateButton"
        x: formatButton.x + formatButton.width + exportPanel.gap
        y: formatButton.y
        width: formatButton.width
        height: Theme.exportFieldHeight
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: activeFocus || exportPanel.exportRateMenuOpen ? exportPanel.accent : exportPanel.line
        activeFocusOnTab: exportPanel.bridge.exports.exportEnabled
        Accessible.role: Accessible.Button
        Accessible.name: "Sample rate: " + exportPanel.exportRateLabels[exportPanel.bridge.exports.exportRateIndex]
        Keys.onReturnPressed: exportPanel.exportRateMenuOpen = !exportPanel.exportRateMenuOpen
        Keys.onSpacePressed: exportPanel.exportRateMenuOpen = !exportPanel.exportRateMenuOpen
        Text {
            x: Theme.exportFieldPadding
            anchors.verticalCenter: parent.verticalCenter
            text: exportPanel.exportRateLabels[exportPanel.bridge.exports.exportRateIndex]
            color: exportPanel.textMain
            font.pixelSize: Theme.typeLabel
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: Theme.exportFieldPadding
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            color: exportPanel.textDim
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            anchors.fill: parent
            enabled: exportPanel.bridge.exports.exportEnabled
            onClicked: {
                exportPanel.exportFormatMenuOpen = false;
                exportPanel.exportRateMenuOpen = !exportPanel.exportRateMenuOpen;
            }
        }
    }
    Text {
        id: rateStatus
        x: exportPanel.margin
        y: formatButton.y + formatButton.height + Theme.spaceSm
        width: exportPanel.width - 2 * exportPanel.margin
        text: exportPanel.bridge.exports.exportRateStatus
        color: exportPanel.bridge.exports.masterExportRateValid ? exportPanel.textDim : exportPanel.amber
        font.pixelSize: Theme.typeSmall
        elide: Text.ElideRight
    }

    Rectangle {
        id: trimTail
        objectName: "exportTrimTailCheck"
        x: exportPanel.margin
        y: rateStatus.y + rateStatus.height + Theme.exportTrimGap
        width: trimRow.implicitWidth
        height: Theme.exportCheckRowHeight
        color: "transparent"
        border.color: activeFocus ? exportPanel.accent : "transparent"
        radius: Theme.radiusMd
        activeFocusOnTab: !exportPanel.bridge.exports.exportBusy
        Accessible.role: Accessible.CheckBox
        Accessible.name: "Trim silence at the end"
        Accessible.checked: exportPanel.bridge.exports.exportTrimTail
        Keys.onSpacePressed: exportPanel.bridge.exports.exportTrimTail = !exportPanel.bridge.exports.exportTrimTail
        Row {
            id: trimRow
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceMd
            Rectangle {
                width: Theme.exportCheckSize
                height: Theme.exportCheckSize
                radius: Theme.radiusMd
                color: exportPanel.bridge.exports.exportTrimTail ? exportPanel.accent : exportPanel.panelRaised
                border.color: exportPanel.bridge.exports.exportTrimTail ? exportPanel.accent : exportPanel.line
                Text {
                    anchors.centerIn: parent
                    visible: exportPanel.bridge.exports.exportTrimTail
                    text: "✓"
                    color: exportPanel.canvas
                    font.pixelSize: Theme.typeBody
                    font.bold: true
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Trim silence at the end"
                color: exportPanel.textMain
                font.pixelSize: Theme.typeLabel
            }
        }
        MouseArea {
            Tip { text: "End the file when the music and its tails have died away" }
            anchors.fill: parent
            enabled: !exportPanel.bridge.exports.exportBusy
            onClicked: exportPanel.bridge.exports.exportTrimTail = !exportPanel.bridge.exports.exportTrimTail
        }
    }

    // Dither for integer formats (ADR-0117): flat TPDF or a noise shape.
    // Float and MP3 are written as rendered, so the choice dims for them.
    Row {
        id: ditherRow
        objectName: "exportDither"
        x: exportPanel.width - exportPanel.margin - width
        y: trimTail.y
        height: Theme.exportCheckRowHeight
        spacing: Theme.spaceSm
        opacity: exportPanel.bridge.exports.exportDithers ? 1 : Theme.exportDisabledOpacity
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: exportPanel.bridge.exports.exportDithers ? "Dither" : "No dither"
            color: exportPanel.textDim
            font.pixelSize: Theme.typeSmall
        }
        Repeater {
            model: exportPanel.bridge.exports.exportDitherLabels
            delegate: Rectangle {
                id: ditherOption
                required property int index
                required property string modelData
                readonly property bool chosen: exportPanel.bridge.exports.exportDitherIndex === index
                objectName: "exportDither" + index
                width: ditherText.implicitWidth + 2 * Theme.spaceMd
                height: parent.height
                radius: height / 2
                color: chosen ? exportPanel.accent : exportPanel.panelRaised
                border.color: activeFocus ? exportPanel.accent : exportPanel.line
                activeFocusOnTab: exportPanel.bridge.exports.exportDithers && !exportPanel.bridge.exports.exportBusy
                Accessible.role: Accessible.RadioButton
                Accessible.name: "Dither: " + modelData
                Accessible.checked: chosen
                Keys.onSpacePressed: exportPanel.bridge.exports.exportDitherIndex = index
                Text {
                    id: ditherText
                    anchors.centerIn: parent
                    text: ditherOption.modelData
                    color: ditherOption.chosen ? exportPanel.canvas : exportPanel.textMain
                    font.pixelSize: Theme.typeSmall
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: exportPanel.bridge.exports.exportDithers && !exportPanel.bridge.exports.exportBusy
                    onClicked: exportPanel.bridge.exports.exportDitherIndex = ditherOption.index
                }
            }
        }
    }

    // Progress while an export runs; the result once it has finished.
    Rectangle {
        id: progressTrack
        objectName: "exportProgress"
        x: exportPanel.margin
        y: trimTail.y + trimTail.height + Theme.spaceXl
        width: exportPanel.width - 2 * exportPanel.margin
        height: Theme.exportProgressHeight
        radius: height / 2
        color: exportPanel.panelRaised
        visible: exportPanel.bridge.exports.exportBusy
        clip: true
        Rectangle {
            id: progressSweep
            width: parent.width / exportPanel.progressSweepParts
            height: parent.height
            radius: height / 2
            color: exportPanel.accent
            NumberAnimation on x {
                from: -progressSweep.width
                to: progressTrack.width
                duration: 1200
                loops: Animation.Infinite
                running: progressTrack.visible && Motion.enabled
            }
        }
    }
    Text {
        id: statusText
        objectName: "exportStatusText"
        x: exportPanel.margin
        y: progressTrack.y + progressTrack.height + Theme.spaceMd
        width: exportPanel.width - 2 * exportPanel.margin
        wrapMode: Text.Wrap
        maximumLineCount: 3
        elide: Text.ElideRight
        text: exportPanel.saved ? "Saved to " + exportPanel.bridge.exports.exportSavedPath
              : exportPanel.bridge.exports.exportStatus.length > 0 ? exportPanel.bridge.exports.exportStatus
              : exportPanel.bridge.exports.exportEnabled ? "Exports the whole arrangement through the master."
              : "Stop playback before exporting."
        color: exportPanel.saved ? exportPanel.textMain : exportPanel.textDim
        font.pixelSize: Theme.typeBody
        lineHeight: Theme.exportLineHeight
    }
    Rectangle {
        id: showInFolder
        objectName: "exportShowInFolderButton"
        x: exportPanel.margin
        y: statusText.y + statusText.height + Theme.spaceMd
        width: showLabel.implicitWidth + Theme.exportButtonPad
        height: visible ? Theme.exportSmallButtonHeight : 0
        visible: exportPanel.saved
        radius: Theme.radiusLg
        color: exportPanel.panelRaised
        border.color: activeFocus ? exportPanel.accent : exportPanel.line
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: "Show the exported file in its folder"
        Keys.onReturnPressed: exportPanel.bridge.exports.show_export_in_folder()
        Keys.onSpacePressed: exportPanel.bridge.exports.show_export_in_folder()
        Text {
            id: showLabel
            anchors.centerIn: parent
            text: "Show in folder"
            color: exportPanel.textMain
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            anchors.fill: parent
            onClicked: exportPanel.bridge.exports.show_export_in_folder()
        }
    }

    Item {
        id: footer
        x: exportPanel.margin
        y: showInFolder.y + showInFolder.height + Theme.exportFooterGap
        width: exportPanel.width - 2 * exportPanel.margin
        height: Theme.exportFooterHeight
        Rectangle {
            id: moreToggle
            objectName: "exportMoreToggle"
            anchors.verticalCenter: parent.verticalCenter
            width: moreLabel.implicitWidth + Theme.spaceLg
            height: Theme.exportToggleHeight
            radius: Theme.radiusLg
            color: "transparent"
            border.color: activeFocus ? exportPanel.accent : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: (exportPanel.moreOpen ? "Hide" : "Show") + " stems and real-time capture"
            Keys.onReturnPressed: exportPanel.moreOpen = !exportPanel.moreOpen
            Keys.onSpacePressed: exportPanel.moreOpen = !exportPanel.moreOpen
            Text {
                id: moreLabel
                anchors.centerIn: parent
                text: (exportPanel.moreOpen ? "▾ " : "▸ ") + "More exports"
                color: exportPanel.textDim
                font.pixelSize: Theme.typeLabel
            }
            MouseArea {
                anchors.fill: parent
                onClicked: exportPanel.moreOpen = !exportPanel.moreOpen
            }
        }
        Rectangle {
            id: cancelButton
            objectName: "exportCancelButton"
            anchors.right: exportButton.left
            anchors.rightMargin: exportPanel.gap
            width: Theme.exportSecondaryButtonWidth
            height: parent.height
            radius: Theme.radiusXl
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: exportPanel.bridge.exports.exportBusy || exportPanel.saved ? "Close" : "Cancel"
            Keys.onReturnPressed: exportPanel.closeRequested()
            Keys.onSpacePressed: exportPanel.closeRequested()
            Text {
                anchors.centerIn: parent
                // An offline export keeps running when the dialog closes.
                text: exportPanel.bridge.exports.exportBusy || exportPanel.saved ? "Close" : "Cancel"
                color: exportPanel.textMain
                font.pixelSize: Theme.typeLabel
            }
            MouseArea {
                anchors.fill: parent
                onClicked: exportPanel.closeRequested()
            }
        }
        Rectangle {
            id: exportButton
            objectName: "exportWavButton"
            anchors.right: parent.right
            width: Theme.exportPrimaryButtonWidth
            height: parent.height
            radius: Theme.radiusXl
            color: exportPanel.masterReady ? exportPanel.accent : exportPanel.panelRaised
            border.color: activeFocus ? Theme.focusOnAccent : exportPanel.masterReady ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: exportPanel.masterReady
            Accessible.role: Accessible.Button
            Accessible.name: "Export " + exportPanel.exportFormatLabels[exportPanel.bridge.exports.exportFormatIndex] + " master"
            Keys.onReturnPressed: exportPanel.exportMaster()
            Keys.onSpacePressed: exportPanel.exportMaster()
            Text {
                anchors.centerIn: parent
                text: exportPanel.bridge.exports.exportBusy ? "Exporting…" : "Export"
                color: exportPanel.masterReady ? Theme.onAccentText : exportPanel.textDim
                font.pixelSize: Theme.typeLabel
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.masterReady
                onClicked: exportPanel.exportMaster()
            }
        }
    }

    // Stems and JACK capture: the less common exports, folded away.
    Item {
        id: moreSection
        x: exportPanel.margin
        y: footer.y + footer.height + Theme.spaceXl
        width: exportPanel.width - 2 * exportPanel.margin
        height: realtimeNote.y + realtimeNote.height
        visible: exportPanel.moreOpen
        Rectangle {
            width: parent.width
            height: Theme.hairline
            color: exportPanel.line
        }
        Caption {
            id: stemCaption
            y: Theme.exportSectionTop
            text: exportPanel.twoInstrumentSources ? "SELECTED SOURCE STEM" : "STEMS"
        }
        Rectangle {
            objectName: "exportStemButton"
            y: stemCaption.y + stemCaption.height + Theme.spaceSm
            width: parent.width
            height: Theme.exportRowHeight
            visible: !exportPanel.twoInstrumentSources
            radius: Theme.radiusLg
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: visible && exportPanel.bridge.exports.exportEnabled
            Accessible.role: Accessible.Button
            Accessible.name: "Export aligned WAV stems to the project folder"
            Keys.onReturnPressed: exportPanel.bridge.exports.export_stems()
            Keys.onSpacePressed: exportPanel.bridge.exports.export_stems()
            Text {
                anchors.centerIn: parent
                text: "Export WAV stems (32-bit float) to the project folder"
                color: exportPanel.bridge.exports.exportEnabled ? exportPanel.textMain : exportPanel.textDim
                font.pixelSize: Theme.typeBody
            }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.exportEnabled
                onClicked: exportPanel.bridge.exports.export_stems()
            }
        }
        Rectangle {
            id: stemSelector
            objectName: "vst3StemSourceSelector"
            y: stemCaption.y + stemCaption.height + Theme.spaceSm
            width: (parent.width - exportPanel.gap) / 2
            height: Theme.exportRowHeight
            radius: Theme.radiusLg
            visible: exportPanel.twoInstrumentSources
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: visible && exportPanel.bridge.exports.exportEnabled && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable
            Accessible.role: Accessible.Button
            Accessible.name: "Selected stem source: " + (exportPanel.selectedStem ? exportPanel.selectedStem.name : "none") +
                             (exportPanel.bridge.exports.twoInstrumentSourceStemAvailable ? "" : ", unavailable: " + exportPanel.bridge.exports.twoInstrumentSourceStemReason)
            Keys.onReturnPressed: exportPanel.stemSourceMenuOpen = !exportPanel.stemSourceMenuOpen
            Keys.onSpacePressed: exportPanel.stemSourceMenuOpen = !exportPanel.stemSourceMenuOpen
            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.spaceSm
                text: (exportPanel.selectedStem ? exportPanel.selectedStem.name : "Select source") + "  ▾"
                color: exportPanel.textMain
                font.pixelSize: Theme.typeBody
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.exportEnabled && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable
                onClicked: exportPanel.stemSourceMenuOpen = !exportPanel.stemSourceMenuOpen
            }
        }
        Rectangle {
            objectName: "vst3StemExportButton"
            x: stemSelector.width + exportPanel.gap
            y: stemSelector.y
            width: stemSelector.width
            height: Theme.exportRowHeight
            radius: Theme.radiusLg
            visible: exportPanel.twoInstrumentSources
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: visible && exportPanel.bridge.exports.exportEnabled && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable
            Accessible.role: Accessible.Button
            Accessible.name: "Export selected source " + (exportPanel.selectedStem ? exportPanel.selectedStem.name : "") + " as WAV 32-bit float to the export file" +
                             (exportPanel.bridge.exports.twoInstrumentSourceStemAvailable ? "" : ", unavailable: " + exportPanel.bridge.exports.twoInstrumentSourceStemReason)
            Keys.onReturnPressed: exportPanel.exportSelectedStem()
            Keys.onSpacePressed: exportPanel.exportSelectedStem()
            Text {
                anchors.centerIn: parent
                text: "Export selected WAV stem"
                color: exportPanel.bridge.exports.exportEnabled && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable ? exportPanel.textMain : exportPanel.textDim
                font.pixelSize: Theme.typeBody
            }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.exportEnabled && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable
                onClicked: exportPanel.exportSelectedStem()
            }
        }
        Text {
            id: stemNote
            y: stemSelector.y + stemSelector.height + Theme.spaceSm
            width: parent.width
            height: visible ? implicitHeight : 0
            visible: exportPanel.twoInstrumentSources
            text: exportPanel.bridge.exports.twoInstrumentSourceStemAvailable ? "One source at a time · .wav target" : exportPanel.bridge.exports.twoInstrumentSourceStemReason
            color: exportPanel.bridge.exports.twoInstrumentSourceStemAvailable ? exportPanel.textDim : exportPanel.amber
            font.pixelSize: Theme.typeSmall
            elide: Text.ElideRight
        }
        Caption {
            id: realtimeCaption
            y: stemNote.y + stemNote.height + Theme.spaceXl
            text: "JACK REAL-TIME · BARS 1–" + exportPanel.bridge.exports.realtimeExportEndBar
        }
        Rectangle {
            id: endBarMinus
            objectName: "realtimeEndBarMinus"
            y: realtimeCaption.y + realtimeCaption.height + Theme.spaceSm
            width: Theme.exportRowHeight; height: Theme.exportRowHeight; radius: Theme.radiusLg
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: exportPanel.bridge.exports.realtimeExportAvailable
            Accessible.role: Accessible.Button
            Accessible.name: "Reduce real-time export end bar"
            Keys.onReturnPressed: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar - 1)
            Keys.onSpacePressed: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar - 1)
            Text { anchors.centerIn: parent; text: "−"; color: exportPanel.textMain; font.pixelSize: Theme.typeHeading }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.realtimeExportAvailable && !exportPanel.bridge.exports.exportBusy
                onClicked: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar - 1)
            }
        }
        Rectangle {
            id: endBarBox
            x: endBarMinus.width + Theme.spaceSm
            y: endBarMinus.y
            width: Theme.exportBarFieldWidth; height: Theme.exportRowHeight; radius: Theme.radiusLg
            color: exportPanel.panelRaised
            border.color: realtimeEndBarInput.activeFocus ? exportPanel.accent : exportPanel.line
            TextInput {
                id: realtimeEndBarInput
                objectName: "realtimeEndBarInput"
                anchors.fill: parent
                anchors.margins: Theme.dialogInputPadding
                text: String(exportPanel.bridge.exports.realtimeExportEndBar)
                validator: IntValidator { bottom: 1; top: 1000000 }
                horizontalAlignment: TextInput.AlignHCenter
                verticalAlignment: TextInput.AlignVCenter
                color: exportPanel.textMain
                font.pixelSize: Theme.typeBody
                enabled: exportPanel.bridge.exports.realtimeExportAvailable && !exportPanel.bridge.exports.exportBusy
                Accessible.name: "Last bar to capture in real time"
                onEditingFinished: exportPanel.bridge.exports.set_realtime_export_end_bar(Number(text))
            }
        }
        Rectangle {
            id: endBarPlus
            objectName: "realtimeEndBarPlus"
            x: endBarBox.x + endBarBox.width + Theme.spaceSm
            y: endBarMinus.y
            width: Theme.exportRowHeight; height: Theme.exportRowHeight; radius: Theme.radiusLg
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: exportPanel.bridge.exports.realtimeExportAvailable
            Accessible.role: Accessible.Button
            Accessible.name: "Increase real-time export end bar"
            Keys.onReturnPressed: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar + 1)
            Keys.onSpacePressed: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar + 1)
            Text { anchors.centerIn: parent; text: "+"; color: exportPanel.textMain; font.pixelSize: Theme.typeHeading }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.realtimeExportAvailable && !exportPanel.bridge.exports.exportBusy
                onClicked: exportPanel.bridge.exports.set_realtime_export_end_bar(exportPanel.bridge.exports.realtimeExportEndBar + 1)
            }
        }
        Rectangle {
            objectName: "realtimeExportButton"
            x: endBarPlus.x + endBarPlus.width + exportPanel.gap
            y: endBarMinus.y
            width: parent.width - x
            height: Theme.exportRowHeight; radius: Theme.radiusLg
            visible: !exportPanel.bridge.exports.realtimeExportBusy
            color: exportPanel.bridge.exports.realtimeExportReady ? exportPanel.accent : exportPanel.panelRaised
            border.color: activeFocus ? Theme.focusOnAccent : exportPanel.line
            activeFocusOnTab: visible && exportPanel.bridge.exports.realtimeExportReady
            Accessible.role: Accessible.Button
            Accessible.name: "Capture bars 1 through " + exportPanel.bridge.exports.realtimeExportEndBar + " as real-time JACK WAV"
            function capture() {
                exportPanel.bridge.exports.set_realtime_export_end_bar(Number(realtimeEndBarInput.text));
                exportPanel.commitTarget();
                exportPanel.bridge.exports.export_realtime_jack();
            }
            Keys.onReturnPressed: capture()
            Keys.onSpacePressed: capture()
            Text {
                anchors.centerIn: parent
                text: "Capture WAV 32-bit float"
                color: exportPanel.bridge.exports.realtimeExportReady ? Theme.onAccentText : exportPanel.textDim
                font.pixelSize: Theme.typeBody
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                enabled: exportPanel.bridge.exports.realtimeExportReady
                onClicked: parent.capture()
            }
        }
        Rectangle {
            objectName: "cancelRealtimeExportButton"
            x: endBarPlus.x + endBarPlus.width + exportPanel.gap
            y: endBarMinus.y
            width: parent.width - x
            height: Theme.exportRowHeight; radius: Theme.radiusLg
            visible: exportPanel.bridge.exports.realtimeExportBusy
            color: exportPanel.panelRaised
            border.color: activeFocus ? exportPanel.accent : exportPanel.line
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "Cancel real-time export"
            Keys.onReturnPressed: exportPanel.bridge.exports.cancel_realtime_export()
            Keys.onSpacePressed: exportPanel.bridge.exports.cancel_realtime_export()
            Text { anchors.centerIn: parent; text: "Cancel real-time capture"; color: exportPanel.textMain; font.pixelSize: Theme.typeBody }
            MouseArea {
                anchors.fill: parent
                onClicked: exportPanel.bridge.exports.cancel_realtime_export()
            }
        }
        Text {
            id: realtimeNote
            y: endBarMinus.y + endBarMinus.height + Theme.spaceMd
            width: parent.width
            wrapMode: Text.WordWrap
            text: exportPanel.bridge.exports.realtimeExportAvailable
                  ? "JACK clocks the exact project-rate range. Sources stop at the end bar; supported effect tails ring out for up to ten seconds. Monitoring depends on your JACK connections. Choose WAV 32-bit float and the project rate."
                  : "JACK real-time capture is unavailable in this build."
            color: exportPanel.textDim
            font.pixelSize: Theme.typeSmall
        }
    }

    // The menus are the panel's last children so they draw over the rows below.
    Rectangle {
        id: stemSourceMenu
        objectName: "vst3StemSourceMenu"
        x: moreSection.x + stemSelector.x
        y: moreSection.y + stemSelector.y + stemSelector.height + Theme.spaceXxs
        z: 25
        width: stemSelector.width
        height: 2 * Theme.spaceXxs + Theme.exportStemOptionHeight * exportPanel.stemSourceTracks.length
        radius: Theme.radiusLg
        visible: exportPanel.moreOpen && exportPanel.twoInstrumentSources && exportPanel.bridge.exports.twoInstrumentSourceStemAvailable && exportPanel.stemSourceMenuOpen
        color: exportPanel.panelRaised
        border.color: exportPanel.accent
        Column {
            anchors.fill: parent
            anchors.margins: Theme.spaceXxs
            Repeater {
                model: exportPanel.stemSourceTracks
                delegate: Rectangle {
                    id: stemOption
                    objectName: "vst3StemSourceOption"
                    required property int index
                    required property var modelData
                    width: parent.width
                    height: Theme.exportStemOptionHeight
                    radius: Theme.radiusMd
                    color: exportPanel.selectedStemIndex === index ? exportPanel.accent : exportPanel.panelRaised
                    activeFocusOnTab: visible
                    Accessible.role: Accessible.Button
                    Accessible.name: "Select stem source " + modelData.name
                    Keys.onReturnPressed: exportPanel.selectStem(index)
                    Keys.onSpacePressed: exportPanel.selectStem(index)
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Theme.spaceXs
                        text: stemOption.modelData.name
                        color: exportPanel.selectedStemIndex === stemOption.index ? exportPanel.canvas : exportPanel.textMain
                        font.pixelSize: Theme.typeBody
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: exportPanel.selectStem(stemOption.index)
                    }
                }
            }
        }
    }
    Rectangle {
        objectName: "masterFormatMenu"
        x: formatButton.x
        y: formatButton.y + formatButton.height + Theme.spaceXxs
        width: formatButton.width
        height: exportPanel.exportFormatLabels.length * Theme.exportMenuOptionHeight + 2 * Theme.spaceXs
        z: 20
        radius: Theme.exportMenuRadius
        color: exportPanel.panelRaised
        border.color: exportPanel.accent
        visible: exportPanel.exportFormatMenuOpen
        Column {
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            Repeater {
                model: exportPanel.exportFormatLabels.length
                delegate: Rectangle {
                    id: formatOption
                    objectName: "masterFormatOption"
                    required property int index
                    width: parent.width
                    height: Theme.exportMenuOptionHeight
                    radius: Theme.radiusMd
                    color: exportPanel.bridge.exports.exportFormatIndex === index ? exportPanel.accent : exportPanel.panelRaised
                    activeFocusOnTab: visible
                    Accessible.role: Accessible.Button
                    Accessible.name: exportPanel.exportFormatLabels[index]
                    Keys.onReturnPressed: exportPanel.selectMasterFormat(index)
                    Keys.onSpacePressed: exportPanel.selectMasterFormat(index)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.exportMenuTextX
                        text: exportPanel.exportFormatLabels[formatOption.index]
                        color: exportPanel.bridge.exports.exportFormatIndex === formatOption.index ? exportPanel.canvas : exportPanel.textMain
                        font.pixelSize: Theme.typeBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: exportPanel.selectMasterFormat(formatOption.index)
                    }
                }
            }
        }
    }
    Rectangle {
        objectName: "exportRateMenu"
        x: rateButton.x
        y: rateButton.y + rateButton.height + Theme.spaceXxs
        width: rateButton.width
        height: exportPanel.exportRateLabels.length * Theme.exportMenuOptionHeight + 2 * Theme.spaceXs
        z: 21
        radius: Theme.exportMenuRadius
        color: exportPanel.panelRaised
        border.color: exportPanel.accent
        visible: exportPanel.exportRateMenuOpen
        Column {
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            Repeater {
                model: exportPanel.exportRateLabels.length
                delegate: Rectangle {
                    id: rateOption
                    objectName: "exportRateOption"
                    required property int index
                    width: parent.width
                    height: Theme.exportMenuOptionHeight
                    radius: Theme.radiusMd
                    readonly property bool rateAvailable: exportPanel.bridge.exports.exportRateAvailability[index]
                    color: exportPanel.bridge.exports.exportRateIndex === index ? exportPanel.accent : exportPanel.panelRaised
                    activeFocusOnTab: visible && rateAvailable
                    Accessible.role: Accessible.Button
                    Accessible.name: exportPanel.exportRateLabels[index] + (rateAvailable ? "" : ", unavailable")
                    Keys.onReturnPressed: exportPanel.selectExportRate(index)
                    Keys.onSpacePressed: exportPanel.selectExportRate(index)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.exportMenuTextX
                        text: exportPanel.exportRateLabels[rateOption.index]
                        color: !rateOption.rateAvailable ? exportPanel.textDim : exportPanel.bridge.exports.exportRateIndex === rateOption.index ? exportPanel.canvas : exportPanel.textMain
                        font.pixelSize: Theme.typeBody
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: rateOption.rateAvailable
                        onClicked: exportPanel.selectExportRate(rateOption.index)
                    }
                }
            }
        }
    }
}
