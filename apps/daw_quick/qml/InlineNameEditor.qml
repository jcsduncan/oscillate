pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A name edited in place over its label: Enter (or clicking away) commits,
// Escape keeps the old name. Hidden until begin().
Rectangle {
    id: editor
    property color textColor: Theme.textMain
    property color accentColor: Theme.accent
    property int pixelSize: Theme.typeEmphasis
    property bool bold: true
    property string original: ""
    // Commits only a changed, non-empty name.
    signal committed(string name)

    visible: false
    radius: Theme.radiusMd
    color: Theme.canvas
    border.color: accentColor
    Accessible.role: Accessible.EditableText
    Accessible.name: "Rename " + original

    function begin(name) {
        original = String(name || "")
        input.text = original
        visible = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function commit() {
        if (!visible)
            return
        // Hide first: losing focus below must not commit twice.
        visible = false
        const name = input.text.trim()
        if (name.length > 0 && name !== original)
            committed(name)
    }
    function cancel() {
        visible = false
    }

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: Theme.fieldPadding
        anchors.rightMargin: Theme.fieldPadding
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        selectByMouse: true
        color: editor.textColor
        selectionColor: editor.accentColor
        selectedTextColor: Theme.canvas
        font.pixelSize: editor.pixelSize
        font.weight: editor.bold ? Font.DemiBold : Font.Normal
        maximumLength: 120
        Keys.onReturnPressed: editor.commit()
        Keys.onEnterPressed: editor.commit()
        Keys.onEscapePressed: editor.cancel()
        onActiveFocusChanged: {
            if (!activeFocus)
                editor.commit()
        }
    }
}
