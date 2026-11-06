import QtQuick
import "../devicekit" as Kit

// A row of latching panel buttons with indicator LEDs, one per choice of a
// choice or stepped parameter (or per entry of `labels`): the kit's
// SegmentedControl in Ember's keys. Left and right arrows move the choice.
Kit.SegmentedControl {
    objectName: "emberButtons:" + parameterId
}
