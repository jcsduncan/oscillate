pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// One loudness gauge: the value, a rail with the target zone shaded and the
// value marked, and a caption. Teal inside the zone, amber within
// `nearMargin` of it, red further out.
Rectangle {
    id: gauge
    required property MixCheckTheme style
    property string gaugeId: ""
    property string title: ""
    property real value: NaN
    property string unit: ""
    property real low: NaN
    property real high: NaN
    // [min, max] drawn along the rail.
    property var rail: [0, 1]
    property string caption: ""
    property bool signedValue: false
    property real nearMargin: 1

    readonly property bool known: isFinite(value)
    readonly property string verdict: {
        if (!known || !isFinite(low) || !isFinite(high))
            return ""
        if (value >= low && value <= high)
            return "ok"
        return value >= low - nearMargin && value <= high + nearMargin ? "warn" : "bad"
    }
    readonly property string valueText: {
        if (!known)
            return "—"
        const shown = style.signedDb(value, 1)
        return (signedValue ? shown : shown.replace(/^\+/, "")) + " " + unit
    }
    function position(v) {
        return Math.max(0, Math.min(1, (v - rail[0]) / (rail[1] - rail[0])))
    }

    objectName: "mixCheckGauge:" + gaugeId
    height: gaugeColumn.implicitHeight + 2 * style.tilePaddingTop
    radius: style.radiusXl
    color: style.panelRaised
    border.color: style.line
    Accessible.role: Accessible.Indicator
    Accessible.name: title + " " + valueText

    Column {
        id: gaugeColumn
        x: gauge.style.tilePadding
        y: gauge.style.tilePaddingTop
        width: parent.width - 2 * gauge.style.tilePadding
        spacing: gauge.style.spaceSm
        Text {
            text: gauge.title.toUpperCase()
            color: gauge.style.textFaint
            font.family: gauge.style.sansFamily
            font.pixelSize: gauge.style.typeSmall
            font.weight: Font.Bold
            font.letterSpacing: gauge.style.headingLetterSpacing
        }
        Text {
            objectName: "mixCheckGaugeValue:" + gauge.gaugeId
            text: gauge.valueText
            color: gauge.style.stateColor(gauge.verdict)
            font.family: gauge.style.monoFamily
            font.pixelSize: gauge.style.figureSize
            font.weight: Font.DemiBold
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.slow } }
        }
        Rectangle {
            id: railView
            width: parent.width
            height: gauge.style.gaugeRailHeight
            radius: gauge.style.radiusSm
            color: gauge.style.wellFill
            border.color: gauge.style.line
            Rectangle {
                visible: isFinite(gauge.low) && isFinite(gauge.high)
                x: visible ? gauge.position(gauge.low) * railView.width : 0
                y: -gauge.style.hairline
                width: visible ? Math.max(1, (gauge.position(gauge.high) - gauge.position(gauge.low)) * railView.width) : 0
                height: railView.height + 2 * gauge.style.hairline
                color: gauge.style.alpha(gauge.style.accent, gauge.style.gaugeZoneAlpha)
                border.color: gauge.style.accent
                Behavior on x { enabled: Motion.enabled; NumberAnimation { duration: Motion.slow; easing.type: Motion.easing } }
                Behavior on width { enabled: Motion.enabled; NumberAnimation { duration: Motion.slow; easing.type: Motion.easing } }
            }
            Rectangle {
                visible: gauge.known
                x: gauge.position(gauge.known ? gauge.value : gauge.rail[0]) * railView.width - Math.floor(gauge.style.gaugeMarkerWidth / 2)
                y: gauge.style.gaugeMarkerTop
                width: gauge.style.gaugeMarkerWidth
                height: gauge.style.gaugeMarkerHeight
                radius: gauge.style.radiusXs
                color: gauge.style.textMain
                Behavior on x { enabled: Motion.enabled; NumberAnimation { duration: Motion.slow; easing.type: Motion.easing } }
            }
        }
        Text {
            width: parent.width
            elide: Text.ElideRight
            text: gauge.caption
            color: gauge.style.textFaint
            font.family: gauge.style.sansFamily
            font.pixelSize: gauge.style.typeBody
        }
    }
}
