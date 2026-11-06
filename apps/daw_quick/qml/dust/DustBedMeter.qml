pragma ComponentBehavior: Bound
import QtQuick

// The bed meter: each layer's level (telemetry, held) as a bar in its tint,
// dimmed while the layer is off, and the duck's gain reduction as a teal
// bar hanging from the top, with its reading.
Item {
    id: meter
    required property var controller
    required property DustTheme theme
    // The layers in telemetry order: [{ key, title }].
    required property var layers
    property var live: ({ live: false })
    readonly property bool playing: live.live === true && live.layers !== undefined
    readonly property int revision: controller.revision
    readonly property var levels: playing ? live.layers : []
    readonly property real duckDb: playing ? Math.max(0, Number(live.duck_db)) : 0
    // The scale: layer bars from the floor to full scale, the duck to its
    // deepest shown reduction.
    readonly property real floorDb: -60
    readonly property real ceilDb: 0
    readonly property real duckRangeDb: 24
    readonly property real tickStepDb: 12
    readonly property real labelHeight: 30
    readonly property real duckWidth: 34
    // Below this the duck's one-decimal reading rounds to nothing.
    readonly property real zeroReadingDb: 0.05

    objectName: "dustBedMeter"
    implicitWidth: theme.bedMeterWidth
    implicitHeight: theme.bedMeterHeight

    function fraction(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / (ceilDb - floorDb)))
    }
    function levelOf(index) {
        return index < levels.length ? Number(levels[index]) : -Infinity
    }

    // The bars' and the duck's top, and the room between them.
    readonly property real barTop: theme.spaceXs
    readonly property real barsWidth: width - duckWidth - theme.spaceXl
    readonly property real barArea: height - labelHeight - 2 * barTop

    // The scale lines behind the bars.
    Repeater {
        model: Math.round((meter.ceilDb - meter.floorDb) / meter.tickStepDb) + 1
        delegate: Item {
            id: tick
            required property int index
            readonly property real db: meter.ceilDb - index * meter.tickStepDb
            y: meter.barTop + (1 - meter.fraction(db)) * meter.barArea
            width: meter.barsWidth
            Rectangle { width: parent.width; height: meter.theme.hairline; color: meter.theme.grid }
            Text {
                x: parent.width - implicitWidth
                y: -implicitHeight - 1
                text: tick.db === 0 ? "0" : "−" + Math.abs(tick.db)
                color: meter.theme.inkFaint
                font.family: meter.theme.mono
                font.pixelSize: meter.theme.meterLabelSize
            }
        }
    }

    Row {
        id: bars
        y: meter.barTop
        width: meter.barsWidth
        height: meter.height - meter.barTop
        readonly property real slot: width / Math.max(1, meter.layers.length)
        Repeater {
            model: meter.layers
            delegate: Item {
                id: column
                required property int index
                required property var modelData
                readonly property string key: modelData.key
                readonly property color tint: meter.theme.layerTint[key]
                readonly property bool on: meter.revision >= 0
                    && meter.controller.choice("dust." + key + ".on") === 1
                readonly property real db: meter.levelOf(index)
                width: bars.slot
                height: bars.height
                opacity: on ? 1 : meter.theme.offOpacity
                Rectangle {
                    id: well
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(meter.theme.bedWellWidth, column.width - meter.theme.bedWellRoom)
                    height: meter.barArea
                    radius: meter.theme.radiusSm
                    color: meter.theme.well
                    border.color: meter.theme.wellEdge
                    Rectangle {
                        objectName: "dustLayerMeter:" + column.key
                        readonly property real fraction: meter.fraction(column.db)
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: meter.theme.hairline
                        x: meter.theme.spaceXxs
                        width: parent.width - 2 * meter.theme.spaceXxs
                        height: fraction * (parent.height - 2 * meter.theme.hairline)
                        radius: meter.theme.radiusXs
                        gradient: Gradient {
                            GradientStop { position: 0; color: Qt.lighter(column.tint, meter.theme.meterTintLighten) }
                            GradientStop { position: 1; color: Qt.darker(column.tint, meter.theme.meterTintDarken) }
                        }
                    }
                }
                DustGlyph {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: meter.barArea + meter.barTop
                    width: meter.theme.meterGlyphSize
                    height: meter.theme.meterGlyphSize
                    theme: meter.theme
                    kind: column.key
                    color: column.on ? column.tint : meter.theme.inkFaint
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: meter.barArea + meter.theme.meterTitleTop
                    text: column.modelData.title.toUpperCase()
                    color: column.on ? meter.theme.inkDim : meter.theme.inkFaint
                    font.pixelSize: meter.theme.meterLabelSize
                    font.bold: true
                    font.letterSpacing: meter.theme.meterLabelSpacing
                }
            }
        }
    }

    // The duck: gain taken from the bed, hanging from the top.
    Item {
        x: meter.width - meter.duckWidth
        y: meter.barTop
        width: meter.duckWidth
        height: meter.height - meter.barTop
        Rectangle {
            id: duckWell
            anchors.horizontalCenter: parent.horizontalCenter
            width: meter.theme.duckWellWidth
            height: meter.barArea
            radius: meter.theme.radiusSm
            color: meter.theme.well
            border.color: meter.theme.wellEdge
            Rectangle {
                objectName: "dustDuckMeter"
                readonly property real reductionDb: meter.duckDb
                x: meter.theme.spaceXxs
                y: meter.theme.hairline
                width: parent.width - 2 * meter.theme.spaceXxs
                height: Math.min(1, reductionDb / meter.duckRangeDb) * (parent.height - 2 * meter.theme.hairline)
                radius: meter.theme.radiusXs
                color: meter.theme.teal
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: meter.barArea + meter.barTop
            text: "DUCK"
            color: meter.theme.teal
            font.pixelSize: meter.theme.meterLabelSize
            font.bold: true
            font.letterSpacing: meter.theme.meterLabelSpacing
        }
        Text {
            objectName: "dustDuckValue"
            anchors.horizontalCenter: parent.horizontalCenter
            y: meter.barArea + meter.theme.duckValueTop
            text: !meter.playing ? "—" : meter.duckDb < meter.zeroReadingDb ? "0.0" : "−" + meter.duckDb.toFixed(1)
            color: meter.theme.ink
            font.family: meter.theme.mono
            font.pixelSize: meter.theme.typeCaption
        }
    }
}
