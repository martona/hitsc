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

        readonly property bool darkMode: launcherTheme.darkMode
        readonly property color window: systemPalette.window
        readonly property color text: systemPalette.windowText
        readonly property color mutedText: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.64)
        readonly property color base: systemPalette.base
        readonly property color button: systemPalette.button
        readonly property color highlightedText: systemPalette.highlightedText
        readonly property color panel: Qt.tint(systemPalette.window, Qt.rgba(systemPalette.text.r, systemPalette.text.g, systemPalette.text.b, darkMode ? 0.08 : 0.04))
        readonly property color panelHover: Qt.tint(panel, Qt.rgba(1, 1, 1, darkMode ? 0.055 : 0.22))
        readonly property color border: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, darkMode ? 0.18 : 0.14)
        readonly property color borderHover: Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, darkMode ? 0.26 : 0.2)
        readonly property color accent: systemPalette.highlight
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
                        id: hostCard

                        anchors.fill: parent
                        radius: 8
                        color: hostHover.hovered ? theme.panelHover : theme.panel
                        border.color: hostHover.hovered ? theme.borderHover : theme.border
                        border.width: 1

                        Behavior on color {
                            ColorAnimation {
                                duration: 110
                                easing.type: Easing.OutCubic
                            }
                        }

                        HoverHandler {
                            id: hostHover
                        }

                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: function(eventPoint, button) {
                                hostMenu.popup(hostCard, eventPoint.position.x, eventPoint.position.y)
                            }
                        }

                        Menu {
                            id: hostMenu

                            palette: root.palette

                            MenuItem {
                                text: "Connect"
                                font.bold: true
                                palette: root.palette
                                onTriggered: {}
                            }

                            MenuItem {
                                text: "Edit"
                                palette: root.palette
                                onTriggered: addHostDialog.openForEdit(hostId)
                            }

                            MenuItem {
                                text: "Delete"
                                palette: root.palette
                                onTriggered: {
                                    const result = hostModel.deleteHost(hostId)
                                    if (!result.ok)
                                        console.error(result.error)
                                }
                            }
                        }

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

                    Item {
                        id: dashedOutline

                        anchors.fill: parent
                        anchors.margins: 4

                        property real dashLength: 14
                        property real dashGap: 9
                        property real thickness: 3.5
                        property real corner: 13
                        property int horizontalDashCount: Math.max(1, Math.floor((width - corner * 2 + dashGap) / (dashLength + dashGap)))
                        property int verticalDashCount: Math.max(1, Math.floor((height - corner * 2 + dashGap) / (dashLength + dashGap)))

                        Repeater {
                            model: dashedOutline.horizontalDashCount

                            Rectangle {
                                x: dashedOutline.corner + index * (dashedOutline.dashLength + dashedOutline.dashGap)
                                y: 0
                                width: Math.min(dashedOutline.dashLength, dashedOutline.width - x - dashedOutline.corner)
                                height: dashedOutline.thickness
                                radius: dashedOutline.thickness / 2
                                color: addHostTile.ghostColor
                            }
                        }

                        Repeater {
                            model: dashedOutline.horizontalDashCount

                            Rectangle {
                                x: dashedOutline.corner + index * (dashedOutline.dashLength + dashedOutline.dashGap)
                                y: dashedOutline.height - dashedOutline.thickness
                                width: Math.min(dashedOutline.dashLength, dashedOutline.width - x - dashedOutline.corner)
                                height: dashedOutline.thickness
                                radius: dashedOutline.thickness / 2
                                color: addHostTile.ghostColor
                            }
                        }

                        Repeater {
                            model: dashedOutline.verticalDashCount

                            Rectangle {
                                x: 0
                                y: dashedOutline.corner + index * (dashedOutline.dashLength + dashedOutline.dashGap)
                                width: dashedOutline.thickness
                                height: Math.min(dashedOutline.dashLength, dashedOutline.height - y - dashedOutline.corner)
                                radius: dashedOutline.thickness / 2
                                color: addHostTile.ghostColor
                            }
                        }

                        Repeater {
                            model: dashedOutline.verticalDashCount

                            Rectangle {
                                x: dashedOutline.width - dashedOutline.thickness
                                y: dashedOutline.corner + index * (dashedOutline.dashLength + dashedOutline.dashGap)
                                width: dashedOutline.thickness
                                height: Math.min(dashedOutline.dashLength, dashedOutline.height - y - dashedOutline.corner)
                                radius: dashedOutline.thickness / 2
                                color: addHostTile.ghostColor
                            }
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

        property bool editing: false
        property string editingHostId: ""
        property bool urlDirty: false
        property string errorText: ""

        modal: true
        closePolicy: Popup.CloseOnEscape
        width: Math.min(root.width - 48, 560)
        padding: 22
        x: Math.round((root.width - width) / 2)
        y: Math.max(24, Math.round((root.height - height) / 2) - 24)

        function selectType(key) {
            for (let i = 0; i < typeCombo.count; ++i) {
                if (typeCombo.model.get(i).key === key) {
                    typeCombo.currentIndex = i
                    return
                }
            }
            typeCombo.currentIndex = 0
        }

        function openFresh() {
            editing = false
            editingHostId = ""
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

        function openForEdit(hostId) {
            const host = hostModel.hostDetails(hostId)
            if (!host.ok) {
                console.error(host.error)
                return
            }

            editing = true
            editingHostId = host.id
            selectType(host.type)
            nameField.text = host.name
            urlField.text = host.url
            usernameField.text = host.username
            passwordField.text = ""
            repeatPasswordField.text = ""
            errorText = ""
            urlDirty = true
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
            spacing: 16

            Label {
                text: addHostDialog.editing ? "Edit host" : "Add host"
                color: theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ComboBox {
                    id: typeCombo

                    textRole: "label"
                    valueRole: "key"
                    model: ListModel {
                        ListElement {
                            key: "megarac"
                            label: "MegaRAC"
                        }
                        ListElement {
                            key: "aten"
                            label: "ATEN"
                        }
                        ListElement {
                            key: "pikvm"
                            label: "PiKVM"
                        }
                    }
                    Layout.preferredWidth: 132
                    Layout.minimumWidth: 120
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
                placeholderText: addHostDialog.editing ? "New password" : "Password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                palette: root.palette
            }

            TextField {
                id: repeatPasswordField
                placeholderText: addHostDialog.editing ? "Repeat new password" : "Repeat password"
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
                    text: addHostDialog.editing ? "Save" : "Add"
                    highlighted: true
                    palette: root.palette
                    onClicked: {
                        const typeKey = typeCombo.currentValue || typeCombo.currentText
                        const result = addHostDialog.editing
                            ? hostModel.updateHost(
                                  addHostDialog.editingHostId,
                                  typeKey,
                                  nameField.text,
                                  urlField.text,
                                  usernameField.text,
                                  passwordField.text,
                                  repeatPasswordField.text)
                            : hostModel.addHost(
                                  typeKey,
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
