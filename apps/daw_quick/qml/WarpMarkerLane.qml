pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Clip-local warp marker lane. The waveform is drawn through the clip's warp
// map (each output position shows the source it plays), over the shared
// quarter-note grid. Solid handles are your reviewed markers; hollow amber
// ticks are the source's saved transient suggestions until you apply or
// dismiss them; hollow blue diamonds preview an auto-warp or conform proposal
// at the positions it would give its markers.
//
// Keyboard: Left/Right move the edit cursor (to the next beat when Snap is
// on); Enter or Insert adds a marker at the cursor; [ and ] select the
// previous or next marker; Alt+Left/Right nudges the selected marker; Delete
// removes it; D dismisses the suggestion nearest the cursor. Mouse:
// double-click adds, drag a handle to move it (one undo step on release),
// right-click a handle to delete it or a suggestion to dismiss it.
Item {
    id: lane
    required property var warpState
    required property var controller
    property bool snap: true
    property bool editable: true
    property color textMain: Theme.warpTextMain
    property color textDim: Theme.warpTextDim
    property color accent: Theme.warpAccent
    property color amber: Theme.warpAmber
    property color lineColor: Theme.warpLine
    property real cursorFraction: 0.5
    // Proposals and marker feet are squares turned to diamonds.
    readonly property real diamondTurn: 45
    property int selectedMarker: -1
    readonly property var markers: warpState.markers || []
    readonly property var suggestions: warpState.suggestions || []
    readonly property var beats: warpState.beats || []
    readonly property var proposed: warpState.proposal ? warpState.proposal.markers : []
    function nearestSuggestion(fraction) {
        let best = -1
        let distance = 1
        for (let index = 0; index < suggestions.length; ++index) {
            const d = Math.abs(suggestions[index].fraction - fraction)
            if (d < distance) { distance = d; best = index }
        }
        return best
    }

    implicitHeight: Theme.warpMarkerLaneHeight

    function snapped(fraction) {
        if (!snap || beats.length === 0) return fraction
        let best = fraction
        let distance = 1
        for (const beat of beats) {
            const d = Math.abs(beat.fraction - fraction)
            if (d < distance) { distance = d; best = beat.fraction }
        }
        return distance * width <= Theme.warpSnapDistance ? best : fraction
    }
    function stepCursor(direction) {
        if (snap && beats.length > 0) {
            const candidates = beats.map(b => b.fraction).filter(f => direction > 0 ? f > cursorFraction + 1e-6 : f < cursorFraction - 1e-6)
            if (candidates.length > 0) {
                cursorFraction = direction > 0 ? Math.min(...candidates) : Math.max(...candidates)
                return
            }
        }
        cursorFraction = Math.max(0.001, Math.min(0.999, cursorFraction + direction / 256))
    }
    function selectMarker(direction) {
        if (markers.length === 0) return
        let index = selectedMarker
        if (index < 0) index = direction > 0 ? 0 : markers.length - 1
        else index = Math.max(0, Math.min(markers.length - 1, index + direction))
        selectedMarker = index
        cursorFraction = markers[index].fraction
    }
    function nudge(direction) {
        if (selectedMarker < 0 || selectedMarker >= markers.length) return
        const current = markers[selectedMarker].fraction
        let target = current + direction / 256
        if (snap && beats.length > 0) {
            const candidates = beats.map(b => b.fraction).filter(f => direction > 0 ? f > current + 1e-6 : f < current - 1e-6)
            if (candidates.length > 0) target = direction > 0 ? Math.min(...candidates) : Math.max(...candidates)
        }
        controller.move_marker(selectedMarker, target)
    }
    onMarkersChanged: if (selectedMarker >= markers.length) selectedMarker = markers.length - 1

    Rectangle {
        id: surface
        objectName: "warpMarkerLane"
        anchors.fill: parent
        radius: Theme.warpStatusRadius
        color: Theme.warpStatusGround
        border.color: activeFocus ? lane.accent : lane.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: "Warp markers"
        Accessible.description: lane.markers.length + " markers, " + lane.suggestions.length
                                + " suggestions. Cursor at " + Math.round(lane.cursorFraction * 100)
                                + " percent of the clip. Enter adds a marker, brackets select markers, "
                                + "Alt arrows move the selected marker, Delete removes it, "
                                + "D dismisses the nearest suggestion"
        // Alt-arrow nudges move a marker, so they need an editable clip.
        Keys.onLeftPressed: (event) => { if (event.modifiers & Qt.AltModifier) { if (lane.editable) lane.nudge(-1) } else lane.stepCursor(-1) }
        Keys.onRightPressed: (event) => { if (event.modifiers & Qt.AltModifier) { if (lane.editable) lane.nudge(1) } else lane.stepCursor(1) }
        Keys.onReturnPressed: if (lane.editable) lane.controller.add_marker(lane.cursorFraction)
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Insert && lane.editable) { lane.controller.add_marker(lane.cursorFraction); event.accepted = true }
            else if (event.key === Qt.Key_BracketLeft) { lane.selectMarker(-1); event.accepted = true }
            else if (event.key === Qt.Key_BracketRight) { lane.selectMarker(1); event.accepted = true }
            else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && lane.selectedMarker >= 0 && lane.editable) {
                lane.controller.remove_marker(lane.selectedMarker); event.accepted = true
            }
            else if (event.key === Qt.Key_D && lane.editable && lane.suggestions.length > 0) {
                lane.controller.dismiss_suggestion(lane.nearestSuggestion(lane.cursorFraction)); event.accepted = true
            }
        }

        Canvas {
            id: canvas
            anchors.fill: parent
            anchors.margins: 1
            property var waveform: lane.warpState.waveform || []
            property var anchors_: lane.warpState.anchors || []
            onWaveformChanged: requestPaint()
            onAnchors_Changed: requestPaint()
            onWidthChanged: requestPaint()
            Connections {
                target: lane
                function onBeatsChanged() { canvas.requestPaint() }
            }
            function sourceAt(output) {
                const a = anchors_
                for (let i = 1; i < a.length; ++i) {
                    if (output <= a[i].output) {
                        const span = a[i].output - a[i - 1].output
                        const t = span > 0 ? (output - a[i - 1].output) / span : 0
                        return a[i - 1].source + t * (a[i].source - a[i - 1].source)
                    }
                }
                return 1
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                for (const beat of lane.beats) {
                    ctx.fillStyle = beat.downbeat ? Theme.warpBeatLineDownbeat : Theme.warpBeatLine
                    ctx.fillRect(Math.round(beat.fraction * width), 0, Theme.hairline, height)
                }
                const columns = waveform.length
                if (columns === 0 || anchors_.length < 2) return
                ctx.fillStyle = Theme.warpWave
                const middle = height / 2
                for (let x = 0; x < width; ++x) {
                    const source = sourceAt(x / width)
                    const column = waveform[Math.min(columns - 1, Math.max(0, Math.floor(source * columns)))]
                    const top = middle - Math.max(0, column.max) * middle * Theme.warpWaveScale
                    const bottom = middle - Math.min(0, column.min) * middle * Theme.warpWaveScale
                    ctx.fillRect(x, top, Theme.hairline, Math.max(Theme.hairline, bottom - top))
                }
            }
        }
        // Edit cursor.
        Rectangle {
            x: lane.cursorFraction * surface.width
            y: 0
            width: Theme.hairline
            height: surface.height
            color: lane.textMain
            opacity: surface.activeFocus ? Theme.warpCursorFocusedOpacity : Theme.warpCursorOpacity
        }
        Repeater {
            model: lane.proposed
            delegate: Rectangle {
                required property var modelData
                objectName: "warpProposed"
                x: modelData.fraction * surface.width - width / 2
                y: surface.height / 2 - height / 2
                width: Theme.warpProposalSize; height: Theme.warpProposalSize
                rotation: lane.diamondTurn
                color: "transparent"
                border.color: lane.accent
                border.width: Theme.warpMarkStroke
            }
        }
        Repeater {
            model: lane.suggestions
            delegate: Item {
                id: suggestion
                required property var modelData
                required property int index
                objectName: "warpSuggestion"
                x: modelData.fraction * surface.width - width / 2
                y: 0
                width: Theme.warpSuggestionSize
                height: surface.height
                MouseArea {
                    anchors.fill: parent
                    enabled: lane.editable
                    acceptedButtons: Qt.RightButton
                    onClicked: lane.controller.dismiss_suggestion(suggestion.index)
                }
                Rectangle { x: Math.floor((parent.width - width) / 2); y: Theme.warpSuggestionSize; width: Theme.hairline; height: parent.height - Theme.warpSuggestionSize; color: lane.amber; opacity: Theme.warpSuggestionStemOpacity }
                Rectangle {
                    x: 0; y: 0; width: Theme.warpSuggestionSize; height: Theme.warpSuggestionSize; radius: width / 2
                    color: "transparent"
                    border.color: lane.amber
                    border.width: Theme.warpMarkStroke
                }
            }
        }
        Repeater {
            model: lane.markers
            delegate: Item {
                id: handle
                required property var modelData
                required property int index
                property real dragFraction: modelData.fraction
                objectName: "warpMarker" + index
                x: dragFraction * surface.width - width / 2
                y: 0
                width: Theme.warpMarkerWidth
                height: surface.height
                Rectangle {
                    x: (parent.width - width) / 2; y: 0; width: Theme.strokeBold; height: parent.height
                    color: lane.selectedMarker === handle.index ? lane.textMain : lane.accent
                }
                Rectangle {
                    x: 0; y: surface.height - Theme.warpMarkerFootBottom; width: Theme.warpMarkerFootSize; height: Theme.warpMarkerFootSize; radius: Theme.radiusXs
                    rotation: lane.diamondTurn
                    color: lane.selectedMarker === handle.index ? lane.textMain : lane.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: lane.editable
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.SizeHorCursor
                    property bool moved: false
                    onPressed: (mouse) => {
                        lane.selectedMarker = handle.index
                        surface.forceActiveFocus()
                        moved = false
                        if (mouse.button === Qt.RightButton) lane.controller.remove_marker(handle.index)
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed) return
                        moved = true
                        const local = mapToItem(surface, mouse.x, mouse.y)
                        handle.dragFraction = lane.snapped(Math.max(0.001, Math.min(0.999, local.x / surface.width)))
                    }
                    onReleased: (mouse) => {
                        if (mouse.button === Qt.LeftButton && moved)
                            lane.controller.move_marker(handle.index, handle.dragFraction)
                        handle.dragFraction = Qt.binding(() => handle.modelData.fraction)
                    }
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            z: -1
            enabled: lane.editable
            onPressed: (mouse) => { surface.forceActiveFocus(); lane.cursorFraction = lane.snapped(mouse.x / surface.width); lane.selectedMarker = -1 }
            onDoubleClicked: (mouse) => lane.controller.add_marker(lane.snapped(mouse.x / surface.width))
        }
    }
}
