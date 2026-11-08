pragma ComponentBehavior: Bound
import QtQuick
import "../shell"
import "MixerDb.js" as Db

// The decibel scale beside a strip's meters and fader: a tick and a label
// at each mark, −∞ at the floor and a longer, brighter unity mark at 0 dB.
// `inset` is the room the fader cap's half-height leaves at each end, so a
// mark sits level with the cap's centre at that level; the meters take the
// same inset.
Item {
    id: scale
    property real inset: 0
    readonly property var levels: Db.marks.concat([Db.floorDb])
    // Where a level sits, in this item's coordinates.
    function yOf(db) { return inset + (height - 2 * inset) * (1 - Db.positionOfDb(db)) }
    implicitWidth: widest.implicitWidth + Theme.mixerScaleUnityLength + Theme.spaceXxs
    Accessible.role: Accessible.Indicator
    Accessible.name: "Decibel scale"

    // The widest label, laid out like the labels; never shown.
    Text {
        id: widest
        visible: false
        text: Db.label(Db.ceilingDb)
        font.pixelSize: Theme.typeCaption
        font.family: Theme.mono
    }
    Repeater {
        model: scale.levels
        Item {
            id: mark
            required property real modelData
            readonly property bool unity: modelData === Db.unityDb
            objectName: "mixerScaleMark:" + Db.label(modelData)
            width: scale.width
            height: label.implicitHeight
            y: scale.yOf(modelData) - height / 2
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: mark.unity ? Theme.mixerScaleUnityLength : Theme.mixerScaleTickLength
                height: mark.unity ? Theme.strokeBold : Theme.hairline
                color: mark.unity ? Theme.mixerScaleUnity : Theme.mixerScaleTick
            }
            Text {
                id: label
                anchors.right: parent.right
                anchors.rightMargin: Theme.mixerScaleUnityLength + Theme.spaceXxs
                anchors.verticalCenter: parent.verticalCenter
                text: Db.label(mark.modelData)
                color: mark.unity ? Theme.mixerScaleUnity : Theme.textFaint
                font.pixelSize: Theme.typeCaption
                font.family: Theme.mono
                font.bold: mark.unity
            }
        }
    }
}
