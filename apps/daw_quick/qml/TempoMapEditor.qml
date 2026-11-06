pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: panel
    required property var bridge
    required property var tempoMap
    required property color panelRaised
    required property color lineColor
    required property color textMain
    required property color textDim
    required property color accent
    required property color amber
    property string selectedTick: ""
    property string selectedPosition: ""
    property bool selectedLast: false
    property int shapeIndex: 0
    readonly property var shapes: ["STEP", "LINEAR BPM", "EASE IN", "EASE OUT", "EASE IN/OUT"]
    // Layout, top to bottom: title and note, the bar / beat / BPM form, shape
    // and apply, the event list (rowStride apart) and the pager.
    readonly property int noteY: 19
    readonly property int formLabelY: 56
    readonly property int formLabelReserve: 50
    readonly property int newButtonY: 51
    readonly property int newButtonWidth: 48
    readonly property int newButtonHeight: 23
    readonly property int fieldsY: 79
    readonly property int fieldHeight: 30
    readonly property int barFieldWidth: 45
    readonly property int slashX: 50
    readonly property int slashY: 87
    readonly property int beatFieldX: 61
    readonly property int beatFieldWidth: 35
    readonly property int bpmFieldX: 101
    readonly property int bpmFieldWidth: 62
    readonly property int actionsY: 115
    readonly property int actionHeight: 29
    readonly property real shapeShare: 0.58
    readonly property int listHeadingY: 151
    readonly property int rowsY: 169
    readonly property int rowStride: 41
    readonly property int rowHeight: 38
    readonly property int rowTextX: 7
    readonly property int rowTextReserve: 83
    readonly property int rowDetailY: 20
    readonly property int rowButtonHeight: 25
    readonly property int editRight: 76
    readonly property int editWidth: 46
    readonly property int deleteRight: 27
    readonly property int deleteWidth: 21
    readonly property int footerLineY: 335
    readonly property int pagerY: 342
    readonly property int pagerWidth: 42
    readonly property int pagerHeight: 22
    readonly property int pageLabelY: 348

    function clearSelection() {
        selectedTick = "";
        selectedPosition = "";
        selectedLast = false;
        shapeIndex = 0;
        bpmField.text = "120";
    }
    function activateForm() {
        const bpm = Number(bpmField.text);
        if (selectedTick.length > 0)
            bridge.update_tempo_event(selectedTick, bpm, shapeIndex);
        else
            bridge.add_tempo_event(Number(barField.text), Number(beatField.text), bpm, shapeIndex);
    }
    function chooseEvent(eventData) {
        selectedTick = eventData.tick;
        selectedPosition = eventData.position;
        selectedLast = eventData.last;
        barField.text = eventData.bar;
        beatField.text = eventData.beat;
        bpmField.text = Number(eventData.bpm).toString();
        shapeIndex = eventData.shapeIndex;
    }

    Text {
        x: 0; y: 0
        text: "PROJECT TEMPO MAP"
        color: panel.textDim
        font.pixelSize: Theme.typeSmall
        font.bold: true
        font.letterSpacing: Theme.tempoMapTitleLetterSpacing
    }
    Text {
        x: parent.width - width; y: 0
        text: panel.tempoMap.eventCount + (panel.tempoMap.eventCount === 1 ? " EVENT" : " EVENTS")
        color: panel.accent
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: 0; y: panel.noteY; width: parent.width
        text: "Musical clips and automation follow this map. Elapsed-pinned material keeps its time."
        wrapMode: Text.WordWrap
        color: panel.textDim
        font.pixelSize: Theme.typeCaption
    }
    Text {
        x: 0; y: panel.formLabelY
        text: panel.selectedTick.length > 0 ? "EDIT " + panel.selectedPosition : "ADD EVENT AT BAR / BEAT"
        color: panel.selectedTick.length > 0 ? panel.accent : panel.textDim
        font.pixelSize: Theme.typeCaption
        font.bold: true
        width: parent.width - panel.formLabelReserve
        elide: Text.ElideRight
    }
    Rectangle {
        id: newMode
        objectName: "tempoNewMode"
        x: parent.width - panel.newButtonWidth; y: panel.newButtonY; width: panel.newButtonWidth; height: panel.newButtonHeight
        radius: Theme.tempoMapControlRadius
        color: panel.panelRaised
        border.color: activeFocus ? panel.accent : panel.lineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Prepare a new tempo event"
        function activate() { panel.clearSelection() }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "NEW"; color: panel.textMain; font.pixelSize: Theme.typeCaption }
        MouseArea {
            anchors.fill: parent
            onClicked: newMode.activate()
            Tip {}
        }
    }
    Rectangle {
        x: 0; y: panel.fieldsY; width: panel.barFieldWidth; height: panel.fieldHeight; radius: Theme.tempoMapControlRadius
        color: panel.panelRaised
        border.color: panel.lineColor
        opacity: panel.selectedTick.length > 0 ? Theme.tempoMapLockedOpacity : 1
        TextInput {
            id: barField
            objectName: "tempoBarInput"
            anchors.fill: parent; anchors.margins: Theme.spaceSm
            text: "3"
            enabled: panel.selectedTick.length === 0
            color: panel.textMain
            font.pixelSize: Theme.typeBody
            selectByMouse: true
            validator: IntValidator { bottom: 1; top: 9999 }
            Accessible.name: "Tempo event bar, 1 to 9999"
        }
    }
    Text { x: panel.slashX; y: panel.slashY; text: "/"; color: panel.textDim; font.pixelSize: Theme.typeLabel }
    Rectangle {
        x: panel.beatFieldX; y: panel.fieldsY; width: panel.beatFieldWidth; height: panel.fieldHeight; radius: Theme.tempoMapControlRadius
        color: panel.panelRaised
        border.color: panel.lineColor
        opacity: panel.selectedTick.length > 0 ? Theme.tempoMapLockedOpacity : 1
        TextInput {
            id: beatField
            objectName: "tempoBeatInput"
            anchors.fill: parent; anchors.margins: Theme.spaceSm
            text: "1"
            enabled: panel.selectedTick.length === 0
            color: panel.textMain
            font.pixelSize: Theme.typeBody
            selectByMouse: true
            validator: IntValidator { bottom: 1; top: 16 }
            Accessible.name: "Tempo event beat"
        }
    }
    Rectangle {
        x: panel.bpmFieldX; y: panel.fieldsY; width: panel.bpmFieldWidth; height: panel.fieldHeight; radius: Theme.tempoMapControlRadius
        color: panel.panelRaised
        border.color: panel.lineColor
        TextInput {
            id: bpmField
            objectName: "tempoBpmInput"
            anchors.fill: parent; anchors.margins: Theme.spaceSm
            text: "120"
            color: panel.textMain
            font.pixelSize: Theme.typeBody
            selectByMouse: true
            validator: DoubleValidator { bottom: 0.001; top: 1000; decimals: 3; locale: "C" }
            Accessible.name: "Tempo event BPM"
        }
    }
    Rectangle {
        id: shapeButton
        objectName: "tempoShapeButton"
        x: 0; y: panel.actionsY; width: parent.width * panel.shapeShare; height: panel.actionHeight
        radius: Theme.tempoMapControlRadius
        color: panel.panelRaised
        border.color: activeFocus ? panel.accent : panel.lineColor
        enabled: !(panel.selectedTick.length > 0 && panel.selectedLast)
        opacity: enabled ? 1 : Theme.tempoMapLockedOpacity
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Shape to next event: " + panel.shapes[panel.shapeIndex]
        function activate() { panel.shapeIndex = (panel.shapeIndex + 1) % panel.shapes.length }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text {
            anchors.centerIn: parent
            text: panel.shapes[panel.shapeIndex] + "  ›"
            color: panel.textMain
            font.pixelSize: Theme.typeCaption
        }
        MouseArea {
            anchors.fill: parent
            enabled: shapeButton.enabled
            onClicked: shapeButton.activate()
            Tip {}
        }
    }
    Rectangle {
        id: applyButton
        objectName: "tempoApplyButton"
        x: parent.width * panel.shapeShare + Theme.spaceSm; y: panel.actionsY
        width: parent.width - x; height: panel.actionHeight
        radius: Theme.tempoMapControlRadius
        color: panel.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: panel.selectedTick.length > 0 ? "Apply tempo event edit" : "Add tempo event"
        function activate() { panel.activateForm() }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text {
            anchors.centerIn: parent
            text: panel.selectedTick.length > 0 ? "APPLY EDIT" : "ADD EVENT"
            color: Theme.tempoMapApplyText
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            onClicked: applyButton.activate()
            Tip {}
        }
    }
    Text {
        x: 0; y: panel.listHeadingY; width: parent.width
        text: "Events · shape controls the transition to the next event"
        color: panel.textDim
        font.pixelSize: Theme.typeCaption
        elide: Text.ElideRight
    }
    Repeater {
        model: panel.tempoMap.events || []
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            objectName: "tempoEventRow"
            x: 0; y: panel.rowsY + index * panel.rowStride
            width: panel.width; height: panel.rowHeight
            radius: Theme.tempoMapControlRadius
            color: panel.selectedTick === modelData.tick ? Theme.tempoMapRowSelected : panel.panelRaised
            border.color: panel.selectedTick === modelData.tick ? panel.accent : panel.lineColor
            Text {
                x: panel.rowTextX; y: Theme.spaceXs; width: parent.width - panel.rowTextReserve
                text: row.modelData.position + "  ·  " + Number(row.modelData.elapsedSeconds).toFixed(2) + "s"
                color: panel.textMain
                font.pixelSize: Theme.typeCaption
                elide: Text.ElideRight
            }
            Text {
                x: panel.rowTextX; y: panel.rowDetailY; width: parent.width - panel.rowTextReserve
                text: Number(row.modelData.bpm).toFixed(2) + " BPM  ·  " + row.modelData.shape
                color: panel.textDim
                font.pixelSize: Theme.typeCaption
                elide: Text.ElideRight
            }
            Rectangle {
                id: editEvent
                objectName: "tempoEditEvent"
                x: parent.width - panel.editRight; y: Theme.spaceSm; width: panel.editWidth; height: panel.rowButtonHeight
                radius: Theme.radiusMd
                color: Theme.tempoMapEditFill
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Edit tempo event at " + row.modelData.position
                function activate() { panel.chooseEvent(row.modelData) }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text { anchors.centerIn: parent; text: "EDIT"; color: panel.textMain; font.pixelSize: Theme.typeCaption }
                MouseArea {
                    anchors.fill: parent
                    onClicked: editEvent.activate()
                    Tip {}
                }
            }
            Rectangle {
                id: removeEvent
                objectName: "tempoRemoveEvent"
                visible: !row.modelData.initial
                x: parent.width - panel.deleteRight; y: Theme.spaceSm; width: panel.deleteWidth; height: panel.rowButtonHeight
                radius: Theme.radiusMd
                color: Theme.tempoMapDeleteFill
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Remove tempo event at " + row.modelData.position
                function activate() {
                    panel.bridge.remove_tempo_event(row.modelData.tick);
                    if (panel.selectedTick === row.modelData.tick)
                        panel.clearSelection();
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text { anchors.centerIn: parent; text: "×"; color: panel.textMain; font.pixelSize: Theme.typeEmphasis }
                MouseArea {
                    anchors.fill: parent
                    onClicked: removeEvent.activate()
                    Tip {}
                }
            }
        }
    }
    Rectangle {
        x: 0; y: panel.footerLineY; width: parent.width; height: Theme.hairline; color: panel.lineColor
    }
    Rectangle {
        id: previousPage
        objectName: "tempoPreviousPage"
        x: 0; y: panel.pagerY; width: panel.pagerWidth; height: panel.pagerHeight
        radius: Theme.radiusMd
        visible: panel.tempoMap.page > 0
        color: panel.panelRaised
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Previous tempo event page"
        function activate() { panel.bridge.set_tempo_event_page(panel.tempoMap.page - 1) }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "‹ PREV"; color: panel.textMain; font.pixelSize: Theme.typeMicro }
        MouseArea {
            anchors.fill: parent
            onClicked: previousPage.activate()
            Tip {}
        }
    }
    Text {
        x: parent.width / 2 - width / 2; y: panel.pageLabelY
        text: "PAGE " + (panel.tempoMap.page + 1) + " / " + panel.tempoMap.pageCount
        color: panel.textDim
        font.pixelSize: Theme.typeMicro
    }
    Rectangle {
        id: nextPage
        objectName: "tempoNextPage"
        x: parent.width - panel.pagerWidth; y: panel.pagerY; width: panel.pagerWidth; height: panel.pagerHeight
        radius: Theme.radiusMd
        visible: panel.tempoMap.page + 1 < panel.tempoMap.pageCount
        color: panel.panelRaised
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Next tempo event page"
        function activate() { panel.bridge.set_tempo_event_page(panel.tempoMap.page + 1) }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "NEXT ›"; color: panel.textMain; font.pixelSize: Theme.typeMicro }
        MouseArea {
            anchors.fill: parent
            onClicked: nextPage.activate()
            Tip {}
        }
    }
}
