pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// The proposal on a timeline: a bar ruler, the sections
// with draggable edges (snapping to 8 bars, then planning again), a row per
// track with where it plays and its Chop fills, the automation lanes, and a
// dot per 16-bar line for the "something new" rule. While the proposal is
// being heard the playhead runs across it; clicking a bar asks to play there.
Item {
    id: timeline
    required property ArrangeTheme style
    property var sections: []
    property var tracks: []
    property var lanes: []
    property var checks: []
    property int lengthBars: 0
    property int phraseBars: 8
    property int changeBars: 16
    // Zero-based bar, or negative when not playing.
    property real playheadBar: -1
    signal edgeMoved(int index, int bar)
    signal playRequested(int bar)

    readonly property real plotWidth: Math.max(1, width - style.gutter)
    readonly property real barWidth: lengthBars > 0 ? plotWidth / lengthBars : 0
    readonly property int rulerHeight: 18
    readonly property int sectionHeight: 24
    readonly property int checkHeight: 16
    readonly property real tracksTop: rulerHeight + sectionHeight + style.spaceSm
    readonly property real lanesTop: tracksTop + tracks.length * style.rowHeight + style.spaceMd
    readonly property real checksTop: lanesTop + lanes.length * (style.laneHeight + style.laneGap) + style.spaceSm

    function xOf(bar) {
        return style.gutter + bar * barWidth
    }

    implicitHeight: checksTop + checkHeight + style.spaceXs
    Accessible.role: Accessible.Chart
    Accessible.name: "The proposed arrangement"

    // Ruler: a number every 16 bars, a faint line every 8.
    Repeater {
        model: timeline.lengthBars > 0 ? Math.floor(timeline.lengthBars / timeline.phraseBars) + 1 : 0
        delegate: Item {
            id: tick
            required property int index
            readonly property int bar: index * timeline.phraseBars
            x: timeline.xOf(bar)
            width: timeline.style.hairline
            height: timeline.height
            Rectangle {
                y: timeline.rulerHeight
                width: timeline.style.hairline
                height: parent.height - timeline.rulerHeight
                color: tick.bar % timeline.changeBars === 0 ? timeline.style.line
                                                            : timeline.style.alpha(timeline.style.line,
                                                                                   timeline.style.minorBarLineAlpha)
            }
            Text {
                visible: tick.bar % timeline.changeBars === 0
                x: timeline.style.rulerLabelInset
                y: timeline.style.spaceXxs
                text: String(tick.bar + 1)
                color: timeline.style.textFaint
                font.family: timeline.style.monoFamily
                font.pixelSize: timeline.style.typeCaption
            }
        }
    }
    MouseArea {
        x: timeline.style.gutter
        width: timeline.plotWidth
        height: timeline.rulerHeight
        onClicked: mouse => timeline.playRequested(Math.floor(mouse.x / Math.max(1, timeline.barWidth)))
        Tip { text: "Hear the proposal from this bar" }
    }

    // Sections, each edge a handle.
    Repeater {
        model: timeline.sections
        delegate: Rectangle {
            id: section
            required property var modelData
            required property int index
            objectName: "arrangeSection:" + index
            x: timeline.xOf(modelData.start) + timeline.style.timelineBlockGap
            y: timeline.rulerHeight
            width: Math.max(0, modelData.bars * timeline.barWidth - 2 * timeline.style.timelineBlockGap)
            height: timeline.sectionHeight
            radius: timeline.style.radiusMd
            color: timeline.style.sectionColor(modelData.kind)
            Text {
                anchors.fill: parent
                anchors.leftMargin: timeline.style.spaceSm
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: section.modelData.name + "  " + section.modelData.bars
                color: timeline.style.textMain
                font.family: timeline.style.sansFamily
                font.pixelSize: timeline.style.typeBody
                font.weight: Font.DemiBold
            }
        }
    }
    Repeater {
        model: timeline.sections
        delegate: Item {
            id: edge
            required property var modelData
            required property int index
            objectName: "arrangeSectionEdge:" + index
            visible: index > 0
            readonly property real home: timeline.xOf(modelData.start)
            x: (handle.pressed ? handle.dragX : home) - width / 2
            y: timeline.rulerHeight
            width: timeline.style.edgeHandleWidth
            height: timeline.height - timeline.rulerHeight
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: handle.pressed || handle.containsMouse ? timeline.style.strokeBold : timeline.style.hairline
                height: parent.height
                color: handle.pressed || handle.containsMouse ? timeline.style.amber
                                                                  : timeline.style.alpha(timeline.style.textFaint,
                                                                                         timeline.style.edgeRestAlpha)
            }
            MouseArea {
                id: handle
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeHorCursor
                preventStealing: true
                property real dragX: edge.home
                onPressed: mouse => dragX = edge.home
                onPositionChanged: mouse => {
                    if (pressed)
                        dragX = Math.max(timeline.style.gutter,
                                         Math.min(timeline.width, edge.mapToItem(timeline, mouse.x, 0).x))
                }
                onReleased: {
                    const bar = Math.round((dragX - timeline.style.gutter) / Math.max(1, timeline.barWidth))
                    if (bar !== edge.modelData.start)
                        timeline.edgeMoved(edge.index, bar)
                }
                Tip { text: "Drag to move where " + edge.modelData.name + " starts (snaps to 8 bars)" }
            }
        }
    }

    // A row per track.
    Repeater {
        model: timeline.tracks
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property color tint: timeline.style.roleColor(modelData.roleIndex)
            objectName: "arrangeRow:" + modelData.id
            y: timeline.tracksTop + index * timeline.style.rowHeight
            width: timeline.width
            height: timeline.style.rowHeight
            opacity: modelData.arranged ? 1 : timeline.style.unarrangedOpacity
            Text {
                width: timeline.style.gutter - timeline.style.gutterLabelGap
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: row.modelData.name
                color: timeline.style.textDim
                font.family: timeline.style.sansFamily
                font.pixelSize: timeline.style.typeBody
            }
            Text {
                visible: !row.modelData.arranged
                x: timeline.style.gutter + timeline.style.spaceSm
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: row.modelData.locked ? "locked · left as it is"
                      : String(row.modelData.unsupported || "").length > 0 ? row.modelData.unsupported
                      : "nothing in the loop"
                color: timeline.style.textFaint
                font.family: timeline.style.monoFamily
                font.pixelSize: timeline.style.timelineLabelSize
            }
            Repeater {
                model: row.modelData.runs || []
                delegate: Rectangle {
                    required property var modelData
                    x: timeline.xOf(modelData.start) + timeline.style.timelineBlockGap
                    y: timeline.style.runInset
                    width: Math.max(1, (modelData.end - modelData.start) * timeline.barWidth
                                       - 2 * timeline.style.timelineBlockGap)
                    height: row.height - 2 * timeline.style.runInset
                    radius: timeline.style.radiusSm
                    color: timeline.style.alpha(row.tint, timeline.style.runFillAlpha)
                    border.color: row.tint
                }
            }
            Repeater {
                model: row.modelData.fills || []
                delegate: Rectangle {
                    required property var modelData
                    x: timeline.xOf(modelData) + timeline.style.timelineBlockGap
                    y: timeline.style.timelineBlockGap
                    width: Math.max(timeline.style.fillMinWidth, timeline.barWidth - 2 * timeline.style.timelineBlockGap)
                    height: row.height - 2 * timeline.style.timelineBlockGap
                    radius: timeline.style.radiusXs
                    color: "transparent"
                    border.color: timeline.style.amber
                    border.width: timeline.style.strokeBold
                }
            }
        }
    }

    // Automation lanes.
    Repeater {
        model: timeline.lanes
        delegate: Item {
            id: lane
            required property var modelData
            required property int index
            readonly property color tint: timeline.style.laneColor(modelData.kind)
            y: timeline.lanesTop + index * (timeline.style.laneHeight + timeline.style.laneGap)
            width: timeline.width
            height: timeline.style.laneHeight
            Text {
                width: timeline.style.gutter - timeline.style.gutterLabelGap
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: lane.modelData.label
                color: lane.tint
                font.family: timeline.style.monoFamily
                font.pixelSize: timeline.style.timelineLabelSize
            }
            Rectangle {
                x: timeline.style.gutter
                width: timeline.plotWidth
                height: parent.height
                color: timeline.style.alpha(lane.tint, timeline.style.laneWashAlpha)
                radius: timeline.style.radiusSm
            }
            Repeater {
                model: lane.modelData.items || []
                delegate: Rectangle {
                    id: item
                    required property var modelData
                    x: timeline.xOf(modelData.start)
                    y: timeline.style.spaceXxs
                    width: Math.max(timeline.style.laneItemMinWidth, (modelData.end - modelData.start) * timeline.barWidth)
                    height: lane.height - 2 * timeline.style.spaceXxs
                    radius: timeline.style.radiusXs
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop {
                            position: 0
                            color: timeline.style.alpha(lane.tint, lane.modelData.kind === "low-pass"
                                                          ? timeline.style.laneRampLowAlpha : timeline.style.laneItemAlpha)
                        }
                        GradientStop {
                            position: 1
                            color: timeline.style.alpha(lane.tint, lane.modelData.kind === "high-pass"
                                                          ? timeline.style.laneRampHighAlpha : timeline.style.laneItemAlpha)
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        Tip { text: item.modelData.label + " · " + item.modelData.detail }
                    }
                }
            }
        }
    }

    // The 16-bar rule.
    Repeater {
        model: timeline.checks
        delegate: Rectangle {
            id: dot
            required property var modelData
            objectName: "arrangeCheck:" + modelData.bar
            readonly property color tint: timeline.style.checkColor(modelData.status)
            x: timeline.xOf(modelData.bar) - width / 2
            y: timeline.checksTop + (timeline.checkHeight - height) / 2
            width: timeline.style.checkDotSize
            height: timeline.style.checkDotSize
            radius: width / 2
            color: tint
            MouseArea {
                anchors.fill: parent
                anchors.margins: -timeline.style.checkDotHitOutset
                hoverEnabled: true
                Tip {
                    text: "Bar " + (dot.modelData.bar + 1) + ": "
                          + (dot.modelData.status === "added" ? "added a change · "
                             : dot.modelData.status === "failed" ? "nothing changes · " : "")
                          + dot.modelData.reason
                }
            }
        }
    }
    Text {
        y: timeline.checksTop
        height: timeline.checkHeight
        verticalAlignment: Text.AlignVCenter
        text: "NEW EVERY 16"
        color: timeline.style.textFaint
        font.family: timeline.style.monoFamily
        font.pixelSize: timeline.style.typeCaption
        font.letterSpacing: timeline.style.arrangeTagSpacing
    }

    // The playhead while the proposal plays.
    Rectangle {
        visible: timeline.playheadBar >= 0 && timeline.playheadBar <= timeline.lengthBars
        x: timeline.xOf(Math.max(0, timeline.playheadBar))
        y: timeline.rulerHeight
        width: timeline.style.strokeBold
        height: timeline.height - timeline.rulerHeight
        color: timeline.style.amber
    }
}
