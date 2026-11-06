pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "CrateContract.js" as Contract

// One pad: its number and key on top, the chop's name,
// and a bar in the chop's kind colour. Pressing it selects the chop and
// plays it until release (Space works the same with focus). It lights while
// its chop sounds and marks the last pad struck. A pad past the last chop
// is empty and does nothing.
Rectangle {
    id: pad
    required property CrateTheme theme
    // The chop index across both banks.
    required property int index
    // The chop's entry from crate.chops, or null.
    property var chop: null
    property bool selected: false
    // The chop sounds now (a playhead in telemetry), and was struck last.
    property bool sounding: false
    property bool last: false
    readonly property bool empty: chop === null
    readonly property color tint: empty ? theme.padEdge : Contract.tagColour(chop.tag)
    readonly property int key: Contract.padKey(index)
    signal pressedPad()
    signal releasedPad()

    objectName: "cratePad:" + index
    implicitHeight: theme.padHeight
    radius: theme.padRadius
    color: selected ? theme.padSelected : theme.padFill
    border.color: selected || sounding ? tint : theme.padEdge
    border.width: selected || sounding ? theme.padActiveEdgeWidth : theme.hairline
    opacity: empty ? theme.padEmptyOpacity : 1
    activeFocusOnTab: !empty
    Accessible.role: Accessible.Button
    Accessible.name: "Pad " + Contract.chopNumber(index) + (empty ? ", empty" : ", " + Contract.chopName(chop))
        + ", " + Contract.noteName(key)
    Accessible.description: (selected ? "Selected. " : "") + "Press to select and play"
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space && !event.isAutoRepeat && !empty) {
            pad.pressedPad()
            event.accepted = true
        }
    }
    Keys.onReleased: event => {
        if (event.key === Qt.Key_Space && !event.isAutoRepeat && !empty) {
            pad.releasedPad()
            event.accepted = true
        }
    }

    // The selection halo: a faint ring in the chop's colour, just outside.
    Rectangle {
        visible: pad.selected
        anchors.fill: parent
        anchors.margins: -pad.theme.padHaloGap
        radius: pad.radius + pad.theme.padHaloGap
        color: "transparent"
        border.width: pad.theme.padHaloWidth
        border.color: Qt.rgba(pad.tint.r, pad.tint.g, pad.tint.b, pad.theme.padHaloAlpha)
    }
    // Lights while its chop sounds.
    Rectangle {
        objectName: "cratePadLive:" + pad.index
        anchors.fill: parent
        radius: pad.radius
        color: pad.tint
        opacity: pad.sounding ? pad.theme.padLiveAlpha : 0
    }

    Item {
        anchors.fill: parent
        anchors.margins: pad.theme.padInset
        Text {
            text: Contract.chopNumber(pad.index)
            color: pad.theme.inkDim
            font.family: pad.theme.mono
            font.pixelSize: pad.theme.captionSize
        }
        // The last pad struck carries a dot beside its number.
        Rectangle {
            objectName: "cratePadLast:" + pad.index
            visible: pad.last
            x: pad.theme.padLastDotX
            y: pad.theme.padLastDotY
            width: pad.theme.padLastDotSize
            height: width
            radius: width / 2
            color: pad.tint
        }
        Text {
            anchors.right: parent.right
            text: Contract.noteName(pad.key)
            color: pad.theme.inkDim
            font.family: pad.theme.mono
            font.pixelSize: pad.theme.captionSize
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            text: pad.empty ? "—" : Contract.chopName(pad.chop)
            elide: Text.ElideRight
            color: pad.empty ? pad.theme.inkFaint : pad.theme.ink
            font.family: pad.theme.sans
            font.pixelSize: pad.theme.bodySize
            font.weight: Font.Medium
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: pad.theme.padBarHeight
            radius: pad.theme.radiusXs
            color: pad.tint
            opacity: pad.selected || pad.sounding ? 1 : pad.theme.padBarOffAlpha
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: !pad.empty
        onPressed: pad.pressedPad()
        onReleased: pad.releasedPad()
        onCanceled: pad.releasedPad()
        Kit.HoverTip { theme: pad.theme }
    }
}
