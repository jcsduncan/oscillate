pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Shown once at startup after Oscillate stopped unexpectedly: where the
// crash report was saved, a way to open its folder, and the bug form to
// attach it to. Nothing is sent; the user attaches the file themselves.
Item {
    id: notice
    required property var report
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    z: Theme.crashNoticeZ
    visible: report.pending

    component NoticeButton: Rectangle {
        id: button
        required property string label
        property bool primary: false
        signal activated()
        width: buttonLabel.implicitWidth + 2 * Theme.spaceLg
        height: Theme.crashNoticeButtonHeight
        radius: Theme.radiusLg
        color: primary ? Theme.toggleOn : Theme.buttonFace
        border.color: activeFocus ? Theme.accent : Theme.controlEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            id: buttonLabel
            anchors.centerIn: parent
            text: button.label
            color: Theme.textMain
            font.pixelSize: Theme.typeLabel
        }
        MouseArea { anchors.fill: parent; onClicked: button.activated() }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.exportScrim
        // Swallows clicks so the shell underneath waits for an answer.
        MouseArea { anchors.fill: parent }
    }
    Rectangle {
        id: card
        objectName: "crashReportCard"
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.space2xl, Theme.crashNoticeWidth)
        height: body.implicitHeight + 2 * Theme.space2xl
        radius: Theme.exportDialogRadius
        color: Theme.panel
        border.color: Theme.line
        Accessible.role: Accessible.Dialog
        Accessible.name: "Oscillate stopped unexpectedly"

        Column {
            id: body
            x: Theme.space2xl
            y: Theme.space2xl
            width: card.width - 2 * Theme.space2xl
            spacing: Theme.spaceLg
            Text {
                text: "Oscillate stopped unexpectedly"
                color: Theme.textMain
                font.pixelSize: Theme.typeDisplay
                font.weight: Font.DemiBold
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Text {
                text: "A crash report was saved on this computer. Nothing has been sent. "
                      + "To report the problem, open the bug form and attach the report. "
                      + "Read it first: its log names your projects, files and plugins."
                color: Theme.textDim
                font.pixelSize: Theme.typeBody
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Rectangle {
                width: parent.width
                height: path.implicitHeight + 2 * Theme.crashNoticePathPadding
                radius: Theme.radiusMd
                color: Theme.canvas
                border.color: Theme.line
                TextEdit {
                    id: path
                    objectName: "crashReportPath"
                    x: Theme.crashNoticePathPadding
                    y: Theme.crashNoticePathPadding
                    width: parent.width - 2 * Theme.crashNoticePathPadding
                    text: notice.report.reportPath
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.textMain
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.typeSmall
                    Accessible.name: "Crash report file"
                }
            }
            Row {
                spacing: Theme.spaceMd
                NoticeButton {
                    objectName: "crashReportShow"
                    label: "Show report"
                    onActivated: notice.report.show_report()
                }
                NoticeButton {
                    objectName: "crashReportBugForm"
                    label: "Open bug form"
                    primary: true
                    onActivated: notice.report.open_bug_form()
                }
                NoticeButton {
                    objectName: "crashReportClose"
                    label: "Close"
                    onActivated: notice.report.dismiss()
                }
            }
        }
    }
    Keys.onEscapePressed: report.dismiss()
}
