import QtQuick
import "../devicekit" as Kit

// A row of mutually exclusive choices for a choice or stepped parameter: the
// kit's SegmentedControl in Prism's look. `icons`, when given, draws a
// PrismIcon per choice instead of its label.
Kit.SegmentedControl {
    id: segmented
    objectName: "prismSegmented:" + parameterId
    icon: Component {
        PrismIcon {
            property color ink
            theme: segmented.theme
            stroke: ink
        }
    }
}
