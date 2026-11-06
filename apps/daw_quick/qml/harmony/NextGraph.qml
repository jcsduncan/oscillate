pragma ComponentBehavior: Bound
import QtQuick

// What comes next, as a radial graph: the selected chord in the middle, the
// likely next chords around it (closer is more likely), each ringed by its
// tension. Click (or Enter) uses one.
Item {
    id: graph
    required property HarmonyTheme style
    required property var controller
    readonly property var candidates: style.list(controller ? controller.next : null)
    readonly property string current: controller ? style.map(controller.selection).name || "" : ""
    readonly property real centre: width / 2
    // The likeliest chord sits on the inner ring, the least likely out on the
    // outer one: the rings' radii as fractions of the graph's width.
    readonly property real nearFraction: 0.25
    readonly property real spanFraction: 0.2
    // Keeps a zero likelihood from dividing by zero.
    readonly property real likelihoodFloor: 0.0001
    readonly property real near: width * nearFraction
    readonly property real span: width * spanFraction
    readonly property real best: candidates.length > 0 ? Math.max(likelihoodFloor, candidates[0].likelihood) : 1

    implicitHeight: width
    function nodeX(index, likelihood) {
        const a = -Math.PI / 2 + index / Math.max(1, candidates.length) * Math.PI * 2
        return centre + Math.cos(a) * (near + (1 - likelihood / best) * span)
    }
    function nodeY(index, likelihood) {
        const a = -Math.PI / 2 + index / Math.max(1, candidates.length) * Math.PI * 2
        return centre + Math.sin(a) * (near + (1 - likelihood / best) * span)
    }

    Canvas {
        id: spokes
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            // Before the panel lays it out the graph has no size to draw in.
            if (!(graph.near > 0))
                return
            ctx.setLineDash(graph.style.graphRingDash)
            ctx.strokeStyle = graph.style.line
            for (const r of [graph.near + graph.span, graph.near]) {
                ctx.beginPath()
                ctx.arc(graph.centre, graph.centre, r, 0, Math.PI * 2)
                ctx.stroke()
            }
            ctx.setLineDash([])
            graph.candidates.forEach((c, i) => {
                const w = c.likelihood / graph.best
                const col = graph.style.tensionColor(c.tension)
                ctx.strokeStyle = Qt.rgba(col.r, col.g, col.b,
                                          graph.style.graphSpokeAlphaMin + graph.style.graphSpokeAlphaRange * w)
                ctx.lineWidth = graph.style.graphSpokeWidthMin + graph.style.graphSpokeWidthRange * w
                ctx.beginPath()
                ctx.moveTo(graph.centre, graph.centre)
                ctx.lineTo(graph.nodeX(i, c.likelihood), graph.nodeY(i, c.likelihood))
                ctx.stroke()
            })
        }
        Connections {
            target: graph
            function onCandidatesChanged() { spokes.requestPaint() }
            function onWidthChanged() { spokes.requestPaint() }
        }
    }
    Rectangle {
        x: graph.centre - width / 2
        y: graph.centre - height / 2
        width: graph.style.graphHubSize; height: width; radius: width / 2
        color: graph.style.panel2
        border.color: graph.style.violet
        border.width: graph.style.strokeBold
        Text {
            anchors.centerIn: parent
            text: graph.current
            color: graph.style.ink
            font.family: graph.style.sans
            font.pixelSize: graph.style.typeEmphasis
            font.weight: Font.Bold
        }
    }
    Repeater {
        model: graph.candidates
        delegate: Rectangle {
            id: node
            required property var modelData
            required property int index
            x: graph.nodeX(index, modelData.likelihood) - width / 2
            y: graph.nodeY(index, modelData.likelihood) - height / 2
            width: graph.style.graphNodeSize; height: width; radius: width / 2
            color: graph.style.panel
            border.color: activeFocus || nodeArea.containsMouse ? graph.style.ink : graph.style.tensionColor(modelData.tension)
            border.width: activeFocus ? graph.style.graphNodeFocusStroke : graph.style.graphNodeStroke
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Use " + modelData.name
            Keys.onReturnPressed: graph.controller.use_next(index)
            Keys.onSpacePressed: graph.controller.use_next(index)
            Column {
                anchors.centerIn: parent
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: node.modelData.name
                    color: graph.style.ink
                    font.family: graph.style.sans
                    font.pixelSize: graph.style.typeBody
                    font.weight: Font.Bold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: node.modelData.roman
                    color: graph.style.dim
                    font.family: graph.style.mono
                    font.pixelSize: graph.style.typeMicro
                }
            }
            MouseArea {
                id: nodeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: graph.controller.use_next(node.index)
            }
        }
    }
}
