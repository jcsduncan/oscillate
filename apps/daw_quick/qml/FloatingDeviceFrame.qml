pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Shared, modeless host for large instrument and effect editors. Only the
// visible surface handles pointer input; the rest of the DAW remains usable.
// A `resizable` frame has a grip at its bottom-right corner and grabs along
// its right and bottom edges; dragging one asks the host for a new size
// (resizeRequested), never below the minimum or past the window, and
// resizeFinished marks the end of the drag so the host can keep the size.
Item {
    id: host
    objectName: "floatingDeviceFrame"
    property string title: "DEVICE"
    property string surfaceObjectName: "floatingDeviceSurface"
    property string dragObjectName: "floatingDeviceDragHandle"
    property real surfaceWidth: Theme.floatingSurfaceWidth
    property real surfaceHeight: Theme.floatingSurfaceHeight
    property real preferredX: -1
    property real preferredY: Theme.floatingPanelTop
    property bool showClose: true
    property color frameColor: Theme.deviceChromeFill
    property color borderColor: Theme.deviceChromeEdge
    property color textColor: Theme.deviceChromeText
    property color accentColor: Theme.deviceChromeAccent
    // The title bar and the chrome's buttons (CLOSE, and a host's own).
    property color titleBarColor: Theme.deviceChromeTitleBar
    property color buttonColor: Theme.deviceChromeButton
    property color buttonHoverColor: Theme.deviceChromeButtonHover
    property color buttonBorderColor: Theme.deviceChromeButtonEdge
    property bool resizable: false
    property real minimumSurfaceWidth: Theme.floatingMinSurfaceWidth
    property real minimumSurfaceHeight: Theme.floatingMinSurfaceHeight
    // The grip's size and the edges' grab width, in pixels.
    property real gripSize: Theme.floatingGripSize
    property real edgeGrab: Theme.floatingEdgeGrab
    readonly property alias contentItem: body
    // The gap kept between the surface and the window's edges.
    readonly property real windowMargin: Theme.spaceLg
    // The title bar, its CLOSE button and the body below it.
    readonly property int titleBarHeight: 34
    readonly property int titleX: 13
    readonly property int titleMinWidth: 40
    readonly property int titleCloseRoom: 118
    readonly property int titleRightRoom: 22
    readonly property int dragCloseRoom: 104
    readonly property int closeWidth: 84
    readonly property int closeHeight: 26
    readonly property int bodyY: 38
    default property alias content: body.data
    signal closeRequested()
    signal resizeRequested(real width, real height)
    signal resizeFinished()

    // A resize handle on the surface. The surface stays where it is while
    // its size changes under the pointer.
    component ResizeArea: MouseArea {
        property bool sizesWidth: true
        property bool sizesHeight: true
        property point start: Qt.point(0, 0)
        property size startSize: Qt.size(0, 0)
        visible: host.resizable
        z: 5
        preventStealing: true
        onPressed: mouse => {
            start = mapToItem(host, mouse.x, mouse.y)
            startSize = Qt.size(surface.width, surface.height)
            surface.x = surface.x
            surface.y = surface.y
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const at = mapToItem(host, mouse.x, mouse.y)
            const maxWidth = host.width - surface.x - host.windowMargin
            const maxHeight = host.height - surface.y - host.windowMargin
            const w = sizesWidth ? startSize.width + at.x - start.x : startSize.width
            const h = sizesHeight ? startSize.height + at.y - start.y : startSize.height
            host.resizeRequested(Math.max(host.minimumSurfaceWidth, Math.min(maxWidth, w)),
                                 Math.max(host.minimumSurfaceHeight, Math.min(maxHeight, h)))
        }
        onReleased: host.resizeFinished()
        onCanceled: host.resizeFinished()
    }

    Rectangle {
        id: surface
        objectName: host.surfaceObjectName
        width: Math.min(host.surfaceWidth, host.width - 2 * host.windowMargin)
        height: Math.min(host.surfaceHeight, host.height - 2 * host.windowMargin)
        x: host.preferredX >= 0
            ? Math.max(host.windowMargin, Math.min(host.preferredX, host.width - width - host.windowMargin))
            : Math.max(host.windowMargin, (host.width - width) / 2)
        y: Math.max(host.windowMargin, Math.min(host.preferredY, host.height - height - host.windowMargin))
        radius: Theme.panelRadius
        color: host.frameColor
        border.color: host.borderColor
        MouseArea { anchors.fill: parent }

        Rectangle {
            id: titleBar
            x: Theme.hairline; y: Theme.hairline; width: parent.width - 2 * Theme.hairline; height: host.titleBarHeight
            radius: Theme.radiusXl
            color: host.titleBarColor
            Text {
                x: host.titleX; anchors.verticalCenter: parent.verticalCenter
                width: Math.max(host.titleMinWidth, parent.width - (host.showClose ? host.titleCloseRoom : host.titleRightRoom))
                elide: Text.ElideRight
                text: host.title.toUpperCase()
                color: host.textColor
                font.pixelSize: Theme.typeBody; font.bold: true; font.letterSpacing: Theme.trackingXs
            }
            MouseArea {
                objectName: host.dragObjectName
                x: 0; y: 0
                width: parent.width - (host.showClose ? host.dragCloseRoom : 0)
                height: parent.height
                cursorShape: Qt.OpenHandCursor
                drag.target: surface
                drag.minimumX: host.windowMargin
                drag.maximumX: Math.max(host.windowMargin, host.width - surface.width - host.windowMargin)
                drag.minimumY: host.windowMargin
                drag.maximumY: Math.max(host.windowMargin, host.height - surface.height - host.windowMargin)
            }
        }

        Rectangle {
            objectName: "floatingDeviceClose"
            visible: host.showClose
            x: parent.width - width - Theme.spaceMd; y: Theme.spaceXs
            width: host.closeWidth; height: host.closeHeight; radius: Theme.controlRadius
            color: host.buttonColor
            border.color: activeFocus ? host.accentColor : host.borderColor
            activeFocusOnTab: host.visible && host.showClose
            Accessible.role: Accessible.Button
            Accessible.name: "Close " + host.title + " editor"
            Keys.onReturnPressed: host.closeRequested()
            Keys.onSpacePressed: host.closeRequested()
            Text { anchors.centerIn: parent; text: "CLOSE"; color: host.textColor; font.pixelSize: Theme.typeSmall }
            MouseArea { anchors.fill: parent; onClicked: host.closeRequested() }
        }

        Item {
            id: body
            objectName: "floatingDeviceBody"
            x: Theme.spaceMd; y: host.bodyY
            width: parent.width - 2 * Theme.spaceMd
            height: parent.height - host.bodyY - Theme.spaceMd
        }

        // Resizing: the corner grip both ways, the edges one way each.
        ResizeArea {
            objectName: "floatingDeviceResizeRight"
            x: parent.width - host.edgeGrab
            y: titleBar.height
            width: host.edgeGrab
            height: parent.height - titleBar.height - host.gripSize
            sizesHeight: false
            cursorShape: Qt.SizeHorCursor
        }
        ResizeArea {
            objectName: "floatingDeviceResizeBottom"
            x: 0
            y: parent.height - host.edgeGrab
            width: parent.width - host.gripSize
            height: host.edgeGrab
            sizesWidth: false
            cursorShape: Qt.SizeVerCursor
        }
        ResizeArea {
            id: grip
            objectName: "floatingDeviceResizeGrip"
            x: parent.width - width
            y: parent.height - height
            width: host.gripSize
            height: host.gripSize
            cursorShape: Qt.SizeFDiagCursor
            Accessible.role: Accessible.Grip
            Accessible.name: "Resize " + host.title
            // Two short diagonal strokes in the corner.
            Canvas {
                anchors.fill: parent
                // Strokes inset from the corner, each a step further out.
                readonly property real cornerInset: 3
                readonly property real strokeStep: 5
                readonly property int strokes: 2
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = host.borderColor
                    ctx.lineWidth = Theme.gripStroke
                    for (let i = 1; i <= strokes; ++i) {
                        ctx.beginPath()
                        ctx.moveTo(width - cornerInset, height - cornerInset - strokeStep * i)
                        ctx.lineTo(width - cornerInset - strokeStep * i, height - cornerInset)
                        ctx.stroke()
                    }
                }
            }
        }
    }
}
