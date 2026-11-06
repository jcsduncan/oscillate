pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "CrateContract.js" as Contract

// The sample's waveform cut into its chops (spec 09 §5): one bar per few
// pixels, coloured by the kind of the chop it falls in, the selected chop
// bright and the rest dim; a numbered marker at each chop's start; and a
// moving line for every chop that sounds (telemetry playheads).
//
// Click a chop to select it (in MANUAL, a click adds a cut there instead).
// Drag a marker to move its cut (one undo step, heard as it moves; Esc
// cancels); double-click a marker to remove it (not the first: a sample
// always plays from its start on pad 1).
Rectangle {
    id: wave
    required property var crate
    required property CrateTheme theme
    // The telemetry frame (crate.hpp layout), empty while nothing sounds.
    property var frame: []

    readonly property var chops: crate !== null ? crate.chops : []
    readonly property int selected: crate !== null ? crate.selected : -1
    readonly property bool manual: crate !== null && crate.chop_mode === "manual"
    readonly property real insetX: 8
    readonly property real insetY: 10
    readonly property real plotWidth: Math.max(1, width - 2 * insetX)
    readonly property int columns: Math.max(1, Math.floor(plotWidth / Contract.barPitch))
    readonly property var peaks: crate !== null && crate.has_sample && crate.sample_name !== null
        ? crate.peaks(columns) : []
    // The marker being dragged, or -1.
    property int dragging: -1
    // move_chop's gesture phases.
    readonly property int phaseBegin: 0
    readonly property int phaseMove: 1
    readonly property int phaseEnd: 2
    readonly property int phaseCancel: 3

    objectName: "crateWaveform"
    radius: theme.cardRadius
    color: theme.well
    border.color: theme.line
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: "Sample waveform with " + chops.length + " chops"

    function xOf(fraction) {
        return insetX + Math.max(0, Math.min(1, fraction)) * plotWidth
    }
    function fractionAt(x) {
        return Math.max(0, Math.min(1, (x - insetX) / plotWidth))
    }
    // The chop that plays `fraction` of the sample, or -1.
    function chopAt(fraction) {
        for (let i = chops.length - 1; i >= 0; --i)
            if (fraction >= chops[i].start)
                return i
        return chops.length > 0 ? 0 : -1
    }

    Canvas {
        id: plot
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = wave.theme
            const mid = height / 2
            ctx.fillStyle = t.centreLine
            ctx.fillRect(0, Math.round(mid), width, t.hairline)
            const peaks = wave.peaks
            const chops = wave.chops
            const n = peaks.length
            if (n === 0)
                return
            const half = mid - wave.insetY
            const step = wave.plotWidth / n
            const barWidth = Math.max(1, step - Contract.barGap)
            let chop = 0
            for (let c = 0; c < n; ++c) {
                const fraction = (c + 0.5) / n
                while (chop + 1 < chops.length && chops[chop + 1].start <= fraction)
                    ++chop
                const tag = chops.length > 0 ? chops[chop].tag : "other"
                const on = chop === wave.selected
                const h = Math.max(1, Math.min(1, Number(peaks[c]) || 0) * half)
                ctx.globalAlpha = on ? t.chopOnAlpha : t.chopOffAlpha
                ctx.fillStyle = Contract.tagColour(tag)
                ctx.fillRect(wave.insetX + c * step, mid - h, barWidth, 2 * h)
            }
            ctx.globalAlpha = 1
            // Every sounding chop's playhead.
            const frame = wave.frame
            if (frame.length >= Contract.T.size) {
                ctx.fillStyle = t.playhead
                for (let i = 0; i < chops.length; ++i) {
                    const p = Number(frame[Contract.T.playheads + chops[i].slot])
                    if (!(p >= 0))
                        continue
                    const x = wave.xOf(chops[i].start + p * (chops[i].end - chops[i].start))
                    ctx.fillRect(Math.round(x), 0, t.strokeBold, height)
                }
            }
        }
        Connections {
            target: wave
            function onPeaksChanged() { plot.requestPaint() }
            function onChopsChanged() { plot.requestPaint() }
            function onSelectedChanged() { plot.requestPaint() }
            function onFrameChanged() { plot.requestPaint() }
            function onWidthChanged() { plot.requestPaint() }
            function onHeightChanged() { plot.requestPaint() }
        }
    }

    // A click selects the chop under it; in MANUAL it adds a cut.
    MouseArea {
        objectName: "crateWaveformArea"
        anchors.fill: parent
        enabled: wave.crate !== null && wave.crate.has_sample
        cursorShape: wave.manual ? Qt.CrossCursor : Qt.PointingHandCursor
        onClicked: mouse => {
            const fraction = wave.fractionAt(mouse.x)
            if (wave.manual) {
                wave.crate.add_chop(fraction)
                return
            }
            const index = wave.chopAt(fraction)
            if (index >= 0)
                wave.crate.selected = index
        }
        Kit.HoverTip {
            theme: wave.theme
            text: wave.manual ? "Click to add a cut · drag a marker to move it · double-click a marker to remove it"
                              : "Click a chop to select it · drag a marker to move its cut"
        }
    }

    // The markers: a count model, so a drag survives the chops updating.
    Repeater {
        model: wave.chops.length
        delegate: Item {
            id: marker
            required property int index
            readonly property var chop: index < wave.chops.length ? wave.chops[index] : null
            readonly property bool on: index === wave.selected
            readonly property color tint: chop !== null ? Contract.tagColour(chop.tag) : wave.theme.ink
            property bool moved: false
            property real pressX: 0
            objectName: "crateMarker:" + index
            x: wave.xOf(chop !== null ? chop.start : 0) - wave.theme.hairline
            width: wave.theme.markerHitWidth
            height: wave.height
            z: on ? 2 : 1
            Accessible.role: Accessible.Button
            Accessible.name: "Chop " + Contract.chopNumber(index) + " marker"

            function cancel() {
                if (wave.dragging === index) {
                    wave.crate.move_chop(index, chop.start, wave.phaseCancel)
                    wave.dragging = -1
                }
                moved = false
            }
            Keys.onEscapePressed: cancel()

            Rectangle {
                width: marker.on ? wave.theme.strokeBold : wave.theme.hairline
                height: parent.height
                color: marker.on ? marker.tint : wave.theme.markerIdle
            }
            Text {
                x: wave.theme.spaceXs
                y: wave.theme.spaceXs
                text: Contract.chopNumber(marker.index)
                color: marker.on ? marker.tint : wave.theme.inkDim
                font.family: wave.theme.mono
                font.pixelSize: wave.theme.labelSize
                font.weight: marker.on ? Font.DemiBold : Font.Normal
            }
            MouseArea {
                anchors.fill: parent
                preventStealing: true
                hoverEnabled: true
                cursorShape: marker.index > 0 ? Qt.SizeHorCursor : Qt.PointingHandCursor
                onPressed: mouse => {
                    marker.forceActiveFocus()
                    marker.pressX = mouse.x
                    marker.moved = false
                }
                onPositionChanged: mouse => {
                    if (!pressed || marker.index === 0)
                        return
                    const at = wave.fractionAt(marker.mapToItem(wave, mouse.x, 0).x)
                    if (!marker.moved) {
                        if (Math.abs(mouse.x - marker.pressX) < Qt.styleHints.startDragDistance)
                            return
                        marker.moved = true
                        wave.dragging = marker.index
                        wave.crate.selected = marker.index
                        wave.crate.move_chop(marker.index, marker.chop.start, wave.phaseBegin)
                    }
                    wave.crate.move_chop(marker.index, at, wave.phaseMove)
                }
                onReleased: mouse => {
                    if (marker.moved && wave.dragging === marker.index) {
                        const at = wave.fractionAt(marker.mapToItem(wave, mouse.x, 0).x)
                        wave.crate.move_chop(marker.index, at, wave.phaseEnd)
                        wave.dragging = -1
                    } else {
                        wave.crate.selected = marker.index
                    }
                    marker.moved = false
                }
                onCanceled: marker.cancel()
                onDoubleClicked: {
                    if (marker.index > 0)
                        wave.crate.remove_chop(marker.index)
                }
                Kit.HoverTip {
                    theme: wave.theme
                    text: marker.index > 0 ? "Chop " + Contract.chopNumber(marker.index)
                                             + " · drag to move · double-click to remove"
                                           : "Chop 01 always starts the sample"
                }
            }
        }
    }
}
