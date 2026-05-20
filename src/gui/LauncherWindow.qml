import QtQuick
import QtQuick.Controls.Fusion
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 980
    height: 680
    minimumWidth: 520
    minimumHeight: 420
    visible: true
    title: "hitsc"

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    QtObject {
        id: theme

        readonly property color window: systemPalette.window
        readonly property color text: systemPalette.windowText
        readonly property color mutedText: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.64)
        readonly property color base: systemPalette.base
        readonly property color button: systemPalette.button
        readonly property color highlightedText: systemPalette.highlightedText
        readonly property color panel: Qt.tint(systemPalette.window, Qt.rgba(systemPalette.text.r, systemPalette.text.g, systemPalette.text.b, darkMode ? 0.08 : 0.04))
        readonly property color border: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, darkMode ? 0.18 : 0.14)
        readonly property color accent: systemPalette.highlight
        readonly property bool darkMode: (systemPalette.window.r * 0.299 + systemPalette.window.g * 0.587 + systemPalette.window.b * 0.114) < 0.5
    }

    function statusColor(status) {
        if (status === "online")
            return "#2fbf71"
        if (status === "offline")
            return "#d75f5f"
        if (status === "checking")
            return "#d8a83e"
        return theme.mutedText
    }

    color: theme.window
    palette.window: theme.window
    palette.windowText: theme.text
    palette.base: theme.base
    palette.alternateBase: theme.panel
    palette.text: theme.text
    palette.button: theme.button
    palette.buttonText: theme.text
    palette.highlight: theme.accent
    palette.highlightedText: theme.highlightedText
    palette.placeholderText: theme.mutedText

    ScrollView {
        id: hostScroll

        anchors.fill: parent
        anchors.margins: 18
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        GridLayout {
            id: hostGrid

            property int columnCount: Math.max(1, Math.floor(width / 288))
            property real tileWidth: Math.max(220, Math.floor((width - Math.max(0, columns - 1) * columnSpacing) / columns))
            property int tileHeight: 154

            width: hostScroll.availableWidth
            columns: columnCount
            rowSpacing: 14
            columnSpacing: 14

            Repeater {
                model: hostModel

                delegate: Item {
                    required property string hostId
                    required property string typeLabel
                    required property string name
                    required property string url
                    required property string host
                    required property string status
                    required property string statusLabel

                    Layout.preferredWidth: hostGrid.tileWidth
                    Layout.preferredHeight: hostGrid.tileHeight

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: theme.panel
                        border.color: theme.border
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Label {
                                    text: name
                                    color: theme.text
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Rectangle {
                                    width: 12
                                    height: 12
                                    radius: 6
                                    color: root.statusColor(status)
                                    border.color: Qt.rgba(0, 0, 0, theme.darkMode ? 0.35 : 0.18)
                                    ToolTip.visible: statusMouse.containsMouse
                                    ToolTip.text: statusLabel

                                    MouseArea {
                                        id: statusMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }
                                }
                            }

                            Label {
                                text: host.length > 0 ? host : url
                                color: theme.mutedText
                                font.pixelSize: 13
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Item {
                                Layout.fillHeight: true
                            }

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: typeLabel
                                    color: theme.text
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Label {
                                    text: statusLabel
                                    color: theme.mutedText
                                    font.pixelSize: 12
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }
                }
            }

            Item {
                id: addHostTile

                property color ghostColor: Qt.rgba(theme.text.r, theme.text.g, theme.text.b, theme.darkMode ? 0.46 : 0.42)

                Layout.preferredWidth: hostGrid.tileWidth
                Layout.preferredHeight: hostGrid.tileHeight

                Rectangle {
                    anchors.fill: parent
                    radius: 13
                    color: theme.window
                    opacity: addHostMouse.containsMouse ? 1.0 : 0.86

                    Canvas {
                        anchors.fill: parent

                        property color strokeColor: addHostTile.ghostColor

                        antialiasing: true
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onStrokeColorChanged: requestPaint()

                        function roundedRectPath(ctx, x, y, w, h, r) {
                            ctx.beginPath()
                            ctx.moveTo(x + r, y)
                            ctx.lineTo(x + w - r, y)
                            ctx.quadraticCurveTo(x + w, y, x + w, y + r)
                            ctx.lineTo(x + w, y + h - r)
                            ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
                            ctx.lineTo(x + r, y + h)
                            ctx.quadraticCurveTo(x, y + h, x, y + h - r)
                            ctx.lineTo(x, y + r)
                            ctx.quadraticCurveTo(x, y, x + r, y)
                            ctx.closePath()
                        }

                        onPaint: {
                            const ctx = getContext("2d")
                            const inset = 4
                            ctx.clearRect(0, 0, width, height)
                            ctx.lineWidth = 3.5
                            ctx.strokeStyle = strokeColor
                            ctx.setLineDash([13, 9])
                            roundedRectPath(ctx, inset, inset, width - inset * 2, height - inset * 2, 13)
                            ctx.stroke()
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        text: "Add Host"
                        color: addHostTile.ghostColor
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addHostMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: addHostDialog.openFresh()
                    }
                }
            }
        }
    }

    Dialog {
        id: addHostDialog

        property bool urlDirty: false
        property string errorText: ""

        modal: true
        closePolicy: Popup.CloseOnEscape
        width: Math.min(root.width - 48, 460)
        x: Math.round((root.width - width) / 2)
        y: Math.max(24, Math.round((root.height - height) / 2) - 24)

        function openFresh() {
            typeCombo.currentIndex = 0
            nameField.text = ""
            urlField.text = "https://"
            usernameField.text = ""
            passwordField.text = ""
            repeatPasswordField.text = ""
            errorText = ""
            urlDirty = false
            open()
            nameField.forceActiveFocus()
        }

        background: Rectangle {
            radius: 8
            color: theme.window
            border.color: theme.border
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: "Add host"
                color: theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }

            ComboBox {
                id: typeCombo
                model: ["megarac", "aten", "pikvm"]
                Layout.fillWidth: true
                palette: root.palette
            }

            TextField {
                id: nameField
                placeholderText: "Name"
                Layout.fillWidth: true
                palette: root.palette
                onTextEdited: {
                    if (!addHostDialog.urlDirty)
                        urlField.text = hostModel.defaultUrlForName(text)
                }
            }

            TextField {
                id: urlField
                placeholderText: "https://host[:port]"
                Layout.fillWidth: true
                palette: root.palette
                inputMethodHints: Qt.ImhUrlCharactersOnly
                onTextEdited: addHostDialog.urlDirty = true
            }

            TextField {
                id: usernameField
                placeholderText: "Username"
                Layout.fillWidth: true
                palette: root.palette
            }

            TextField {
                id: passwordField
                placeholderText: "Password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                palette: root.palette
            }

            TextField {
                id: repeatPasswordField
                placeholderText: "Repeat password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                palette: root.palette
            }

            Label {
                text: addHostDialog.errorText
                visible: text.length > 0
                color: "#d75f5f"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    text: "Cancel"
                    palette: root.palette
                    onClicked: addHostDialog.close()
                }

                Button {
                    text: "Add"
                    highlighted: true
                    palette: root.palette
                    onClicked: {
                        const result = hostModel.addHost(
                            typeCombo.currentText,
                            nameField.text,
                            urlField.text,
                            usernameField.text,
                            passwordField.text,
                            repeatPasswordField.text)
                        if (result.ok) {
                            addHostDialog.close()
                        } else {
                            addHostDialog.errorText = result.error
                        }
                    }
                }
            }
        }
    }
}
