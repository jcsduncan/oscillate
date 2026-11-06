pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import "shell"

Item {
    id: panel
    required property var review
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber
    required property color lineColor
    readonly property var preview: review.preview
    property int draftTonic: 0
    property int draftMode: 0
    // Rows from the top.
    readonly property int detectedY: 24
    readonly property int confidenceY: 48
    readonly property int compatibleY: 66
    readonly property int compatibleHeight: 32
    readonly property int correctionY: 102
    readonly property int controlsY: 128
    readonly property int controlHeight: 27
    readonly property int tonicWidth: 53
    readonly property int modeX: 60
    readonly property int modeWidth: 75
    readonly property int applyX: 142
    readonly property int applyMinWidth: 65
    readonly property int statusY: 164
    readonly property int statusHeight: 28
    readonly property int actionsY: 197
    readonly property int actionHeight: 24
    readonly property int clearWidth: 105
    readonly property int reanalyzeWidth: 70
    readonly property var tonicNames: Contracts.pitch_class_names(Contracts.ascii)
    onPreviewChanged: {
        draftTonic = preview.correctedTonic >= 0 ? preview.correctedTonic
                   : preview.detectedTonic >= 0 ? preview.detectedTonic : 0
        draftMode = preview.correctedMode >= 0 ? preview.correctedMode
                  : preview.detectedMode >= 0 ? preview.detectedMode : 0
    }
    Text {
        objectName: "keyReviewTitle"
        x: 0; y: 0
        text: "KEY REVIEW"
        color: panel.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.trackingLg
    }
    Text {
        x: parent.width - width; y: 0
        text: panel.preview.cacheDegraded ? "CACHE OFF" : panel.preview.cacheHit ? "CACHED" : "PROJECT"
        color: panel.preview.cacheDegraded ? panel.amber : panel.accent
        font.pixelSize: Theme.typeCaption
    }
    Text {
        objectName: "keyGeneratedValue"
        x: 0; y: panel.detectedY
        width: parent.width
        text: panel.preview.state === "loading" ? "ANALYZING…" : "Suggested: " + panel.preview.detected
        color: panel.textMain
        font.pixelSize: Theme.typeSubtitle
        font.bold: true
        elide: Text.ElideRight
    }
    Text {
        objectName: "keyConfidenceValue"
        x: 0; y: panel.confidenceY
        text: "CONFIDENCE " + Math.round(panel.preview.confidence * 100) + "%"
        color: panel.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        objectName: "keyCompatibleValues"
        x: 0; y: panel.compatibleY
        width: parent.width
        height: panel.compatibleHeight
        text: panel.preview.compatible.length > 0
              ? panel.preview.compatible.map(item => item.relation + ": " + item.key).join("  ·  ")
              : "No compatible-key suggestions"
        color: panel.textDim
        font.pixelSize: Theme.typeCaption
        wrapMode: Text.WordWrap
    }
    Text {
        objectName: "keyCorrectionValue"
        x: 0; y: panel.correctionY
        width: parent.width
        text: panel.preview.corrected.length > 0 ? "Your key: " + panel.preview.corrected : "Your key: not set"
        color: panel.preview.corrected.length > 0 ? panel.accent : panel.textDim
        font.pixelSize: Theme.typeSmall
        elide: Text.ElideRight
    }
    Rectangle {
        x: 0; y: panel.controlsY; width: panel.tonicWidth; height: panel.controlHeight
        radius: Theme.controlRadius
        color: Theme.keyReviewControlFill
        border.color: activeFocus ? panel.accent : panel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Choose key tonic"
        Keys.onReturnPressed: panel.draftTonic = (panel.draftTonic + 1) % panel.tonicNames.length
        Keys.onSpacePressed: panel.draftTonic = (panel.draftTonic + 1) % panel.tonicNames.length
        Text { anchors.centerIn: parent; text: panel.tonicNames[panel.draftTonic] + " ▾"; color: panel.textMain; font.pixelSize: Theme.typeSmall }
        MouseArea {
            anchors.fill: parent
            onClicked: panel.draftTonic = (panel.draftTonic + 1) % panel.tonicNames.length
            Tip {}
        }
    }
    Rectangle {
        x: panel.modeX; y: panel.controlsY; width: panel.modeWidth; height: panel.controlHeight
        radius: Theme.controlRadius
        color: Theme.keyReviewControlFill
        border.color: activeFocus ? panel.accent : panel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Choose major or minor"
        Keys.onReturnPressed: panel.draftMode = 1 - panel.draftMode
        Keys.onSpacePressed: panel.draftMode = 1 - panel.draftMode
        Text { anchors.centerIn: parent; text: panel.draftMode === 0 ? "MAJOR ▾" : "MINOR ▾"; color: panel.textMain; font.pixelSize: Theme.typeCaption }
        MouseArea {
            anchors.fill: parent
            onClicked: panel.draftMode = 1 - panel.draftMode
            Tip {}
        }
    }
    Rectangle {
        objectName: "keyApplyCorrection"
        x: panel.applyX; y: panel.controlsY; width: Math.max(panel.applyMinWidth, parent.width - panel.applyX); height: panel.controlHeight
        radius: Theme.controlRadius
        color: panel.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Apply manual key correction"
        Keys.onReturnPressed: panel.review.set_correction(panel.draftTonic, panel.draftMode)
        Keys.onSpacePressed: panel.review.set_correction(panel.draftTonic, panel.draftMode)
        Text { anchors.centerIn: parent; text: "APPLY"; color: Theme.keyReviewApplyText; font.pixelSize: Theme.typeCaption; font.bold: true }
        MouseArea {
            anchors.fill: parent
            onClicked: panel.review.set_correction(panel.draftTonic, panel.draftMode)
            Tip {}
        }
    }
    Text {
        objectName: "keyReviewStatus"
        x: 0; y: panel.statusY
        width: parent.width; height: panel.statusHeight
        text: panel.preview.message
        color: panel.preview.state === "unavailable" || panel.preview.state === "stale" ? panel.amber : panel.textDim
        font.pixelSize: Theme.typeCaption
        wrapMode: Text.WordWrap
    }
    Rectangle {
        objectName: "keyClearCorrection"
        x: 0; y: panel.actionsY; width: panel.clearWidth; height: panel.actionHeight
        radius: Theme.controlRadius
        color: Theme.keyReviewControlFill
        border.color: activeFocus ? panel.accent : panel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Clear manual key correction"
        Keys.onReturnPressed: panel.review.clear_correction()
        Keys.onSpacePressed: panel.review.clear_correction()
        Text { anchors.centerIn: parent; text: "CLEAR YOUR KEY"; color: panel.textDim; font.pixelSize: Theme.typeCaption }
        MouseArea {
            anchors.fill: parent
            onClicked: panel.review.clear_correction()
            Tip {}
        }
    }
    Rectangle {
        objectName: "keyReanalyze"
        x: parent.width - panel.reanalyzeWidth; y: panel.actionsY; width: panel.reanalyzeWidth; height: panel.actionHeight
        radius: Theme.controlRadius
        color: Theme.keyReviewControlFill
        border.color: activeFocus ? panel.accent : panel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Reanalyze selected source key"
        Keys.onReturnPressed: panel.review.reanalyze()
        Keys.onSpacePressed: panel.review.reanalyze()
        Text { anchors.centerIn: parent; text: "REANALYZE"; color: panel.textDim; font.pixelSize: Theme.typeCaption }
        MouseArea {
            anchors.fill: parent
            onClicked: panel.review.reanalyze()
            Tip {}
        }
    }
}
