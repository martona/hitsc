import QtQuick
import QtQuick.Controls.Fusion
import QtQuick.Layouts

// Compact, mstsc-style connect surface: pick or type a host, reuse saved
// credentials or enter new ones (optionally saving them), and connect.
Item {
    id: panel

    property var theme
    property var controlPalette
    property var statusColor

    property bool _connecting: false

    function _typeIndex(key) {
        for (var i = 0; i < typeCombo.model.count; ++i) {
            if (typeCombo.model.get(i).key === key)
                return i
        }
        return 0
    }

    function _applySelection() {
        var host = picker.selectedHost
        if (host) {
            var details = hostModel.hostDetails(host.id)
            usernameField.text = (details && details.ok) ? details.username : ""
            typeCombo.currentIndex = _typeIndex(host.typeKey)
        } else {
            usernameField.text = ""
            typeCombo.currentIndex = 0
        }
        passwordField.text = ""
        saveCredentials.checked = false
        errorLabel.text = ""
    }

    function _connect() {
        if (panel._connecting || picker.text.trim().length === 0)
            return
        panel._connecting = true
        var host = picker.selectedHost
        var result = hostModel.quickConnect(
            host ? host.id : "",
            picker.text,
            typeCombo.currentValue || "auto",
            usernameField.text,
            passwordField.text,
            saveCredentials.checked)
        if (result.ok) {
            Qt.quit()    // mini mode is a one-shot launcher; the child outlives us
        } else {
            errorLabel.text = result.error
            panel._connecting = false
        }
    }

    // On show: focus the host field and seed it (fully selected) with the
    // last-connected host, mstsc-style — creds/type prefill via the selection
    // change, and the dropdown stays closed.
    function _onShown() {
        picker.focusField()
        var last = hostModel.lastConnectedHost()
        if (last.length > 0)
            picker.presetHost(last)
    }

    Component.onCompleted: if (visible)
        Qt.callLater(panel._onShown)

    onVisibleChanged: if (visible)
        Qt.callLater(panel._onShown)

    ColumnLayout {
        anchors.top: parent.top
        anchors.topMargin: 28
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 56, 384)
        spacing: 12

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: 6
            spacing: 12

            Image {
                source: "qrc:/icons/hitsc-64.png"
                sourceSize.width: 48
                sourceSize.height: 48
            }

            Label {
                text: "hitsc"
                color: panel.theme.text
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }
        }

        HostPicker {
            id: picker

            Layout.fillWidth: true
            theme: panel.theme
            controlPalette: panel.controlPalette
            statusColor: panel.statusColor

            onSelectedHostChanged: panel._applySelection()
            onIsNewChanged: panel._applySelection()
            onAccepted: panel._connect()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: "Type"
                color: panel.theme.mutedText
                Layout.preferredWidth: 64
            }

            ComboBox {
                id: typeCombo

                Layout.fillWidth: true
                textRole: "label"
                valueRole: "key"
                palette: panel.controlPalette

                Keys.onReturnPressed: function(event) {
                    if (typeCombo.popup.visible) {
                        event.accepted = false
                    } else {
                        panel._connect()
                        event.accepted = true
                    }
                }
                Keys.onEnterPressed: function(event) {
                    if (typeCombo.popup.visible) {
                        event.accepted = false
                    } else {
                        panel._connect()
                        event.accepted = true
                    }
                }

                model: ListModel {
                    ListElement {
                        key: "auto"
                        label: "Auto"
                    }
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
            }
        }

        TextField {
            id: usernameField

            Layout.fillWidth: true
            placeholderText: "Username"
            palette: panel.controlPalette
            onAccepted: panel._connect()
        }

        TextField {
            id: passwordField

            Layout.fillWidth: true
            placeholderText: (picker.selectedHost && picker.selectedHost.hasCredentials)
                ? "Using saved password — type to override" : "Password"
            echoMode: TextInput.Password
            palette: panel.controlPalette
            onAccepted: panel._connect()
        }

        Label {
            id: errorLabel

            Layout.fillWidth: true
            Layout.preferredHeight: 20
            color: "#d75f5f"
            elide: Text.ElideRight
            clip: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            CheckBox {
                id: saveCredentials

                text: "Save credentials"
                palette: panel.controlPalette

                Keys.onReturnPressed: panel._connect()
                Keys.onEnterPressed: panel._connect()
            }

            Item {
                Layout.fillWidth: true
            }

            Button {
                text: "Connect"
                highlighted: true
                enabled: picker.text.trim().length > 0
                palette: panel.controlPalette
                onClicked: panel._connect()
            }
        }
    }
}
