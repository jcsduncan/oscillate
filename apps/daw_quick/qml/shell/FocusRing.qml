import QtQuick

// The keyboard focus ring: an accent stroke just outside a control, shown
// while it holds keyboard focus. For controls that are not a rectangle with
// an edge of their own to light (a readout, a pill whose edge is already
// accent, a list). It rings its parent unless given a sibling as `target`.
Rectangle {
    objectName: "focusRing"
    property Item target: parent
    // The target's corner radius, so the ring follows its shape.
    property real targetRadius: Theme.controlRadius
    anchors.fill: target
    anchors.margins: -Theme.focusRingOffset
    radius: targetRadius + Theme.focusRingOffset
    color: "transparent"
    border.color: Theme.focusRing
    border.width: Theme.focusRingWidth
    visible: target !== null && target.activeFocus
}
