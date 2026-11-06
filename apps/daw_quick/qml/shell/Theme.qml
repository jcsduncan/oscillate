pragma Singleton
import QtQuick
import "../devicekit" as Kit

// The shell's tokens as one shared instance (devicekit/ShellTheme): shell
// QML reads colours, type and sizes from here instead of literals.
Kit.ShellTheme {}
