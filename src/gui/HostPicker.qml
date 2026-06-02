import QtQuick
import QtQuick.Controls.Fusion
import QtQuick.Layouts

// Editable host picker: filters the saved-host list as you type, inline-completes
// the first hostname-prefix match (suffix selected after the caret), and reports
// whether the committed text is an existing host or a brand-new one.
Item {
    id: control

    property var theme
    property var controlPalette
    property var statusColor

    // Outputs.
    property string text: ""
    property var selectedHost: null
    property bool isNew: false

    signal accepted()

    property var _results: []
    property bool _suppress: false
    property string _typedPrefix: ""

    implicitWidth: 260
    implicitHeight: field.implicitHeight

    function _refresh(applyCompletion) {
        var typed = field.text
        control._typedPrefix = typed
        var results = hostModel.searchHosts(typed)
        control._results = results
        listView.currentIndex = -1

        if (applyCompletion && typed.length > 0 && results.length > 0
                && results[0].matchKind === "hostPrefix") {
            var full = results[0].host
            if (full.length > typed.length
                    && full.toLowerCase().substring(0, typed.length) === typed.toLowerCase()) {
                field.text = typed + full.substring(typed.length)
                field.select(typed.length, full.length)
                listView.currentIndex = 0
            }
        }

        _syncSelection()

        if (results.length > 0 && field.activeFocus)
            popup.open()
        else
            popup.close()
    }

    function _syncSelection() {
        var current = field.text
        var match = null
        for (var i = 0; i < control._results.length; ++i) {
            if (control._results[i].host.toLowerCase() === current.toLowerCase()) {
                match = control._results[i]
                break
            }
        }
        control.selectedHost = match
        control.isNew = current.trim().length > 0 && match === null
        control.text = current
    }

    // Mirror the list highlight into the edit box: show the highlighted host with
    // the originally-typed prefix left unselected and the remainder selected.
    function _previewCurrent() {
        if (listView.currentIndex < 0 || listView.currentIndex >= control._results.length) {
            field.text = control._typedPrefix
            field.cursorPosition = field.text.length
            _syncSelection()
            return
        }

        var hostText = control._results[listView.currentIndex].host
        var prefix = control._typedPrefix
        field.text = hostText
        if (prefix.length > 0 && prefix.length <= hostText.length
                && hostText.toLowerCase().substring(0, prefix.length) === prefix.toLowerCase())
            field.select(prefix.length, hostText.length)
        else
            field.select(0, hostText.length)
        listView.positionViewAtIndex(listView.currentIndex, ListView.Contain)
        _syncSelection()
    }

    function _choose(hostText) {
        field.text = hostText
        listView.currentIndex = -1
        _syncSelection()
        popup.close()
    }

    function focusField() {
        field.forceActiveFocus()
    }

    // Seed the field with a known host, fully selected, without opening the
    // dropdown. Resolves the selection so the panel can prefill creds/type.
    function presetHost(hostText) {
        control._typedPrefix = hostText
        field.text = hostText
        control._results = hostModel.searchHosts(hostText)
        listView.currentIndex = -1
        _syncSelection()
        popup.close()
        field.selectAll()
    }

    TextField {
        id: field

        anchors.fill: parent
        palette: control.controlPalette
        placeholderText: "host or host:port"
        inputMethodHints: Qt.ImhUrlCharactersOnly

        onTextEdited: control._refresh(!control._suppress)

        onActiveFocusChanged: if (!activeFocus)
            popup.close()

        Keys.onPressed: function(event) {
            control._suppress = (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)

            if (event.key === Qt.Key_Down) {
                if (control._results.length === 0)
                    control._refresh(false)
                if (control._results.length > 0) {
                    popup.open()
                    listView.currentIndex =
                        Math.min(listView.currentIndex + 1, control._results.length - 1)
                    if (listView.currentIndex < 0)
                        listView.currentIndex = 0
                    control._previewCurrent()
                }
                event.accepted = true
            } else if (event.key === Qt.Key_Up) {
                if (listView.currentIndex >= 0) {
                    listView.currentIndex -= 1
                    control._previewCurrent()
                }
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (popup.opened && listView.currentIndex >= 0
                        && listView.currentIndex < control._results.length)
                    control._choose(control._results[listView.currentIndex].host)
                else
                    popup.close()
                control.accepted()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape && popup.opened) {
                popup.close()
                event.accepted = true
            }
        }
    }

    Popup {
        id: popup

        y: field.height + 2
        width: control.width
        padding: 1
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        implicitHeight: Math.min(224, listView.contentHeight + 2)

        background: Rectangle {
            color: control.theme.base
            border.color: control.theme.border
            border.width: 1
            radius: 6
        }

        contentItem: ListView {
            id: listView

            clip: true
            model: control._results
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds

            delegate: ItemDelegate {
                id: row

                required property var modelData
                required property int index

                width: ListView.view.width
                height: 30
                leftPadding: 10
                rightPadding: 10
                topPadding: 0
                bottomPadding: 0
                focusPolicy: Qt.NoFocus

                background: Rectangle {
                    color: (row.ListView.isCurrentItem || row.hovered)
                        ? control.theme.panelHover : "transparent"
                }

                onClicked: {
                    control._choose(row.modelData.host)
                    field.forceActiveFocus()
                }

                contentItem: RowLayout {
                    spacing: 10

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        Layout.alignment: Qt.AlignVCenter
                        color: control.statusColor(row.modelData.status)
                    }

                    Label {
                        text: row.modelData.host
                        color: control.theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Label {
                        text: row.modelData.typeLabel
                        color: control.theme.mutedText
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
