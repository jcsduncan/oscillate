pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Warp markers on a clip's waveform (ADR-0112), on the arrangement's
// selected clip and in the docked clip editor. Along the top band the
// pointer shows a ghost snapped to the nearest transient or beat (Alt: any
// sample) and a click pins a marker there. Dragging a marker stretches only
// the audio between its neighbours, with each side's tempo shown live;
// Shift-drag keeps the marker on its beat and slides the audio under it;
// double-click removes it; right-click lists the marker commands. Every
// gesture is one undo step, and `preview` lets the host redraw the
// waveform while it is dragged.
Item {
    id: overlay
    objectName: "warpMarkerOverlay"
    required property var bridge
    required property string clipId
    // The part of the clip this item spans, as fractions of its length.
    required property real viewFrom
    required property real viewTo
    required property bool editable
    required property string monoFamily
    // The band that pins markers (the arrangement); the clip editor pins
    // with its own click and leaves this off.
    property bool pinBand: true

    readonly property var warp: bridge.clipWarp.state
    readonly property bool current: warp.clipId === clipId
    readonly property var markers: current ? (warp.markers || []) : []
    readonly property var tempo: bridge.clipTempo.clips[clipId]
    readonly property color gold: Theme.clipEditorMarker
    readonly property int beatsPerBar: 4
    readonly property real msPerSecond: 1000
    // Absorbs rounding when a position lands on a beat.
    readonly property real beatEpsilon: 1e-6
    // The grid check (transients) and each stretch's tempo.
    property var check: ({})
    property var segments: []
    // A drag in progress: the marker, "stretch" or "slide", and where to
    // (stretch: the marker's new place; slide: the audio brought under it).
    property int dragIndex: -1
    property string dragMode: ""
    property real dragTo: 0
    property real hoverAt: -1
    property bool hoverOnTransient: false
    property int menuIndex: -1
    property real menuX: 0
    // While dragging: output anchors `prev` and `next` stay, and the audio
    // that played at `from` plays at `to`. Null otherwise.
    readonly property var preview: {
        if (dragIndex < 0 || dragIndex >= markers.length)
            return null
        const at = markers[dragIndex].fraction
        const bounds = neighbours(dragIndex)
        return dragMode === "slide"
            ? { prev: bounds.prev, next: bounds.next, from: dragTo, to: at }
            : { prev: bounds.prev, next: bounds.next, from: at, to: dragTo }
    }

    function refresh() {
        check = clipId.length > 0 ? bridge.clipTempo.inspect(clipId) : ({})
        segments = clipId.length > 0 ? bridge.clipTempo.segments(clipId) : []
    }
    onClipIdChanged: { closeMenu(); refresh() }
    onTempoChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: overlay.bridge.clipWarp
        function onChanged() { overlay.refresh() }
    }

    function fractionAt(x) { return viewFrom + x / Math.max(1, width) * (viewTo - viewFrom) }
    function xAt(fraction) { return (fraction - viewFrom) / Math.max(1e-9, viewTo - viewFrom) * width }
    function neighbours(index) {
        return { prev: index > 0 ? markers[index - 1].fraction : 0,
                 next: index + 1 < markers.length ? markers[index + 1].fraction : 1 }
    }
    // Keeps a dragged position a couple of pixels inside its neighbours.
    function within(index, fraction) {
        const bounds = neighbours(index)
        const gap = Theme.warpMarkerMinGap * (viewTo - viewFrom) / Math.max(1, width)
        return Math.max(bounds.prev + gap, Math.min(bounds.next - gap, fraction))
    }
    // The transient nearest `x` within the snap distance, else the nearest
    // beat; `free` (Alt) takes the pointer's own position.
    function snapped(x, free) {
        const at = fractionAt(x)
        hoverOnTransient = false
        if (free)
            return at
        let best = -1
        let distance = Theme.clipEditorSnapDistance
        for (const onset of (check.transients || [])) {
            const d = Math.abs(xAt(onset.fraction) - x)
            if (d < distance) {
                distance = d
                best = onset.fraction
            }
        }
        if (best >= 0) {
            hoverOnTransient = true
            return best
        }
        if (tempo && tempo.beat > 0 && tempo.downbeat >= 0)
            return tempo.downbeat + Math.round((at - tempo.downbeat) / tempo.beat) * tempo.beat
        return at
    }
    // "bar.beat" of the clip's own grid at a fraction, or "".
    function beatLabel(fraction) {
        if (!tempo || !(tempo.beat > 0) || tempo.downbeat < 0)
            return ""
        const beats = (fraction - tempo.downbeat) / tempo.beat + beatEpsilon
        const bar = Math.floor(beats / beatsPerBar)
        const beat = beats - bar * beatsPerBar
        return (bar + 1) + "." + (Math.floor(beat) + 1)
    }
    // The clip bar (1-based) a fraction lies in, for Fix drift.
    function barAt(fraction) {
        if (!tempo || !(tempo.beat > 0) || tempo.downbeat < 0)
            return 1
        return Math.max(1, Math.floor((fraction - tempo.downbeat) / (beatsPerBar * tempo.beat)) + 1)
    }
    function segmentBpm(fromFraction) {
        for (const segment of segments)
            if (Math.abs(segment.from - fromFraction) < 1e-6)
                return segment.bpm
        return 0
    }
    // The segment tempo while a marker is stretched: each side plays its
    // source over the new span instead of the old.
    function stretchedBpm(index, side) {
        if (index >= markers.length)
            return 0
        const bounds = neighbours(index)
        const at = markers[index].fraction
        if (side < 0) {
            const bpm = segmentBpm(bounds.prev)
            return bpm * (dragTo - bounds.prev) / Math.max(1e-9, at - bounds.prev)
        }
        const bpm = segmentBpm(at)
        return bpm * (bounds.next - dragTo) / Math.max(1e-9, bounds.next - at)
    }
    function closeMenu() { menuIndex = -1 }

    // The pin band: hover for the ghost, click to pin.
    MouseArea {
        id: band
        objectName: "warpPinBand"
        visible: overlay.pinBand
        enabled: overlay.editable && overlay.current
        width: parent.width
        height: Theme.warpBandHeight
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        Accessible.role: Accessible.Button
        Accessible.name: "Warp marker band: click to pin a marker at the nearest transient or beat, Alt for any sample"
        onExited: overlay.hoverAt = -1
        onPositionChanged: mouse => overlay.hoverAt = overlay.snapped(mouse.x, (mouse.modifiers & Qt.AltModifier) !== 0)
        onClicked: mouse => {
            const at = overlay.snapped(mouse.x, (mouse.modifiers & Qt.AltModifier) !== 0)
            overlay.closeMenu()
            if (at > 0 && at < 1)
                overlay.bridge.clipWarp.add_marker(at)
        }
    }

    // The ghost where a click pins.
    Item {
        objectName: "warpMarkerGhost"
        visible: overlay.pinBand && overlay.hoverAt >= 0 && overlay.dragIndex < 0
        x: overlay.xAt(overlay.hoverAt)
        height: overlay.height
        Rectangle { width: Theme.hairline; height: parent.height; color: Theme.clipEditorHoverGhost }
        Rectangle {
            x: Theme.spaceSm
            y: Theme.warpBandHeight + Theme.warpTagGap
            width: ghostText.implicitWidth + Theme.spaceMd
            height: Theme.warpTagHeight
            radius: Theme.radiusSm
            color: Theme.warpTagGround
            Text {
                id: ghostText
                anchors.centerIn: parent
                text: (overlay.hoverOnTransient ? "transient" : "beat")
                    + (overlay.beatLabel(overlay.hoverAt).length > 0 ? " · beat " + overlay.beatLabel(overlay.hoverAt) : "")
                color: overlay.gold
                font.pixelSize: Theme.typeMicro
                font.family: overlay.monoFamily
            }
        }
    }

    Repeater {
        model: overlay.markers
        delegate: Item {
            id: marker
            objectName: "warpMarker"
            required property var modelData
            required property int index
            readonly property bool dragged: overlay.dragIndex === index
            readonly property real at: dragged && overlay.dragMode === "stretch" ? overlay.dragTo : modelData.fraction
            visible: at >= overlay.viewFrom && at <= overlay.viewTo
            x: overlay.xAt(at)
            height: overlay.height
            z: 2
            Column {
                height: parent.height
                spacing: Theme.clipEditorMarkerDash
                Repeater {
                    model: Math.ceil(marker.height / (2 * Theme.clipEditorMarkerDash))
                    delegate: Rectangle { width: Theme.hairline; height: Theme.clipEditorMarkerDash; color: overlay.gold }
                }
            }
            Rectangle {
                // A square turned to a diamond.
                readonly property real diamondTurn: 45
                x: -(Theme.warpMarkerHeadSize - Theme.hairline) / 2
                y: (Theme.warpBandHeight - Theme.warpMarkerHeadSize) / 2
                width: Theme.warpMarkerHeadSize
                height: Theme.warpMarkerHeadSize
                rotation: diamondTurn
                color: overlay.gold
            }
            // The segment's tempo; while stretching, both sides' live.
            Repeater {
                model: marker.dragged && overlay.dragMode === "stretch" ? [-1, 1] : [1]
                delegate: Rectangle {
                    id: segTag
                    objectName: "warpSegmentTag"
                    required property int modelData
                    readonly property real bpm: marker.dragged && overlay.dragMode === "stretch"
                        ? overlay.stretchedBpm(marker.index, modelData) : overlay.segmentBpm(marker.modelData.fraction)
                    visible: bpm > 0
                    x: modelData < 0 ? -width - Theme.spaceSm : Theme.spaceSm
                    y: Theme.warpBandHeight + Theme.warpTagGap
                    width: segText.implicitWidth + Theme.spaceMd
                    height: Theme.warpTagHeight
                    radius: Theme.radiusSm
                    color: overlay.gold
                    Text {
                        id: segText
                        anchors.centerIn: parent
                        text: "seg " + segTag.bpm.toFixed(2)
                        color: Theme.canvas
                        font.pixelSize: Theme.typeMicro
                        font.family: overlay.monoFamily
                    }
                }
            }
            // Sliding: how far the audio moves.
            Rectangle {
                objectName: "warpSlideTag"
                readonly property real ms: overlay.tempo
                    ? (marker.modelData.fraction - overlay.dragTo) * overlay.tempo.seconds * overlay.msPerSecond : 0
                visible: marker.dragged && overlay.dragMode === "slide"
                x: Theme.spaceSm
                y: Theme.warpBandHeight + Theme.warpTagGap
                width: slideText.implicitWidth + Theme.spaceMd
                height: Theme.warpTagHeight
                radius: Theme.radiusSm
                color: Theme.warpTagGround
                Text {
                    id: slideText
                    anchors.centerIn: parent
                    text: (parent.ms >= 0 ? "→ audio slides " : "← audio slides ") + Math.abs(parent.ms).toFixed(1) + " ms"
                    color: Theme.chipText
                    font.pixelSize: Theme.typeMicro
                    font.family: overlay.monoFamily
                }
            }
            MouseArea {
                objectName: "warpMarkerHandle"
                x: -width / 2
                width: Theme.clipEditorMarkerGrabWidth
                height: Math.max(Theme.warpBandHeight, Theme.clipEditorMarkerGrabHeight)
                enabled: overlay.editable
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.SizeHorCursor
                preventStealing: true
                Accessible.role: Accessible.Slider
                Accessible.name: "Warp marker " + (marker.index + 1) + ": drag to stretch, Shift-drag to slide the audio, double-click to remove, right-click for more"
                property real pressX: 0
                onPressed: mouse => {
                    overlay.closeMenu()
                    if (mouse.button === Qt.RightButton) {
                        overlay.menuIndex = marker.index
                        overlay.menuX = marker.x
                        return
                    }
                    pressX = mapToItem(overlay, mouse.x, mouse.y).x
                    overlay.dragMode = (mouse.modifiers & Qt.ShiftModifier) !== 0 ? "slide" : "stretch"
                    overlay.dragTo = marker.modelData.fraction
                    overlay.dragIndex = marker.index
                }
                onPositionChanged: mouse => {
                    if (!pressed || overlay.dragIndex !== marker.index)
                        return
                    const x = mapToItem(overlay, mouse.x, mouse.y).x
                    if (overlay.dragMode === "slide") {
                        // Dragging right brings earlier audio under the marker.
                        const shift = (x - pressX) / Math.max(1, overlay.width) * (overlay.viewTo - overlay.viewFrom)
                        overlay.dragTo = overlay.within(marker.index, marker.modelData.fraction - shift)
                    } else {
                        overlay.dragTo = overlay.within(marker.index, overlay.snapped(x, (mouse.modifiers & Qt.AltModifier) !== 0))
                    }
                }
                onReleased: {
                    if (overlay.dragIndex !== marker.index)
                        return
                    const index = overlay.dragIndex
                    const to = overlay.dragTo
                    const mode = overlay.dragMode
                    overlay.dragIndex = -1
                    if (Math.abs(to - marker.modelData.fraction) * overlay.width / Math.max(1e-9, overlay.viewTo - overlay.viewFrom) < Theme.warpMarkerMinGap)
                        return
                    if (mode === "slide")
                        overlay.bridge.clipWarp.slide_marker(index, to)
                    else
                        overlay.bridge.clipWarp.move_marker(index, to)
                }
                onCanceled: overlay.dragIndex = -1
                onDoubleClicked: mouse => {
                    if (mouse.button === Qt.LeftButton)
                        overlay.bridge.clipWarp.remove_marker(marker.index)
                }
            }
        }
    }

    // The marker commands, from a right-click.
    Rectangle {
        id: menu
        objectName: "warpMarkerMenu"
        visible: overlay.menuIndex >= 0 && overlay.menuIndex < overlay.markers.length
        readonly property real at: visible ? overlay.markers[overlay.menuIndex].fraction : 0
        readonly property bool last: overlay.menuIndex + 1 === overlay.markers.length
        x: Math.max(0, Math.min(overlay.width - width, overlay.menuX + Theme.spaceSm))
        y: Theme.warpBandHeight
        z: 5
        width: Theme.warpMenuWidth
        height: menuColumn.implicitHeight + 2 * Theme.warpMenuPadding
        radius: Theme.radiusLg
        color: Theme.panelRaised
        border.color: Theme.controlEdge
        Accessible.role: Accessible.PopupMenu
        Accessible.name: "Warp marker commands"
        Keys.onEscapePressed: overlay.closeMenu()
        Column {
            id: menuColumn
            x: Theme.warpMenuPadding
            y: Theme.warpMenuPadding
            width: parent.width - 2 * Theme.warpMenuPadding
            Repeater {
                model: [
                    { key: "downbeat", name: "Set downbeat here", hint: "" },
                    { key: "straight", name: "Straight from here", hint: "" },
                    { key: "tempo", name: "Set tempo from this segment", hint: "" },
                    { key: "drift", name: "Fix drift from here", hint: "" },
                    { key: "remove", name: "Remove marker", hint: "Double-click" },
                    { key: "after", name: "Remove all after", hint: "" }
                ]
                delegate: Rectangle {
                    id: entry
                    required property var modelData
                    readonly property bool active: overlay.editable
                        && !(modelData.key === "after" && menu.last)
                        && !(modelData.key === "tempo" && overlay.segmentBpm(menu.at) <= 0)
                        && !(modelData.key === "drift" && !(overlay.tempo && overlay.tempo.follow))
                    objectName: "warpMenu_" + modelData.key
                    width: menuColumn.width
                    height: Theme.warpMenuEntryHeight
                    radius: Theme.radiusMd
                    color: entryArea.containsMouse && active ? Theme.menuHover : "transparent"
                    opacity: active ? 1 : Theme.clipEditorInactiveOpacity
                    Accessible.role: Accessible.MenuItem
                    Accessible.name: modelData.name
                    function activate() {
                        const index = overlay.menuIndex
                        const at = menu.at
                        const chosen = active
                        overlay.closeMenu()
                        if (!chosen)
                            return
                        switch (modelData.key) {
                        case "downbeat": overlay.bridge.clipTempo.set_downbeat(overlay.clipId, at, true); break
                        case "straight": overlay.bridge.clipWarp.straighten_from(index); break
                        case "tempo": overlay.bridge.clipTempo.set_bpm_from_segment(overlay.clipId, index); break
                        case "drift": overlay.bridge.clipTempo.fix_drift(overlay.clipId, overlay.barAt(at)); break
                        case "remove": overlay.bridge.clipWarp.remove_marker(index); break
                        case "after": overlay.bridge.clipWarp.remove_markers_after(index); break
                        }
                    }
                    Text {
                        x: Theme.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.name
                        color: Theme.chipText
                        font.pixelSize: Theme.typeSmall
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.hint
                        color: Theme.textDim
                        font.pixelSize: Theme.typeMicro
                    }
                    MouseArea {
                        id: entryArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: entry.activate()
                    }
                }
            }
        }
    }
}
