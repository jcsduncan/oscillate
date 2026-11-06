pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The export dialog: a centred card over the shell, opened by the top
// bar's Export button, Ctrl+Shift+E and the command palette. Exporting
// starts from its Export button; an offline export keeps running when the
// dialog closes, and the dialog shows its progress and result when reopened.
Item {
    id: dialog
    Reveal { target: dialog }
    required property var bridge
    required property color canvas
    required property color panel
    required property color panelRaised
    required property color line
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber
    property alias exportFormatMenuOpen: content.exportFormatMenuOpen
    property alias exportRateMenuOpen: content.exportRateMenuOpen
    property alias moreOpen: content.moreOpen
    signal closeRequested()

    onVisibleChanged: {
        if (!visible)
            return;
        content.closeMenus();
        bridge.exports.refresh_export_target();
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.exportScrim
        MouseArea { anchors.fill: parent; onClicked: dialog.closeRequested() }
    }
    Rectangle {
        id: card
        objectName: "exportDialogCard"
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.space2xl, Theme.exportDialogMaxWidth)
        height: Math.min(parent.height - 2 * Theme.space2xl, scroller.y + content.implicitHeight)
        radius: Theme.exportDialogRadius
        color: dialog.panel
        border.color: dialog.line
        Accessible.role: Accessible.Dialog
        Accessible.name: "Export"
        // Clicks inside close an open menu, never the dialog.
        MouseArea { anchors.fill: parent; onClicked: content.closeMenus() }
        Text {
            x: Theme.space2xl
            y: Theme.exportTitleY
            text: "Export"
            color: dialog.textMain
            font.pixelSize: Theme.typeDisplay
            font.weight: Font.DemiBold
        }
        Text {
            x: Theme.space2xl
            y: Theme.exportProjectY
            text: dialog.bridge.projectName
            color: dialog.textDim
            font.pixelSize: Theme.typeBody
            width: card.width - Theme.exportProjectRight
            elide: Text.ElideRight
        }
        Rectangle {
            objectName: "exportDialogCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: Theme.exportCloseInset
            y: Theme.exportCloseInset
            width: Theme.exportCloseSize
            height: Theme.exportCloseSize
            radius: Theme.exportCloseRadius
            color: dialog.panelRaised
            border.color: activeFocus ? dialog.accent : dialog.line
            activeFocusOnTab: dialog.visible
            Accessible.role: Accessible.Button
            Accessible.name: "Close export"
            Keys.onReturnPressed: dialog.closeRequested()
            Keys.onSpacePressed: dialog.closeRequested()
            Text { anchors.centerIn: parent; text: "×"; color: dialog.textMain; font.pixelSize: Theme.closeGlyphSize }
            MouseArea {
                anchors.fill: parent
                onClicked: dialog.closeRequested()
                Tip { shortcut: "Esc" }
            }
        }
        Flickable {
            id: scroller
            y: Theme.exportBodyY
            width: card.width
            height: card.height - y
            contentWidth: width
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ExportPanel {
                id: content
                width: scroller.width
                height: implicitHeight
                bridge: dialog.bridge
                canvas: dialog.canvas
                panelRaised: dialog.panelRaised
                line: dialog.line
                textMain: dialog.textMain
                textDim: dialog.textDim
                accent: dialog.accent
                amber: dialog.amber
                onCloseRequested: dialog.closeRequested()
            }
        }
    }
}
