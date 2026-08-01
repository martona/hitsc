import QtQuick
import QtQuick.Controls.Fusion
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Window

ApplicationWindow {
    id: root

    property string viewMode: windowPlacement.mode

    width: 980
    height: 680
    visible: false
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

    function connectHost(hostId) {
        const result = hostModel.connectHost(hostId)
        if (!result.ok)
            console.error(result.error)
    }

    function coldResetHost(hostId) {
        const result = hostModel.coldResetHost(hostId)
        if (!result.ok)
            console.error(result.error)
    }

    function toggleMaximized() {
        root.visibility = root.visibility === Window.Maximized ? Window.Windowed : Window.Maximized
    }

    // A single Win11-style caption button. Registered with QWindowKit as a system
    // button in launcher_gui.cpp (which keeps onClicked working — see the QWindowKit
    // QtQuick example). Glyphs come from the Segoe MDL2 Assets icon font.
    component CaptionButton: Button {
        id: capBtn

        property string sym: ""
        property bool isClose: false
        property color fg: "black"
        property bool dark: false

        Layout.fillHeight: true
        implicitWidth: 46
        focusPolicy: Qt.NoFocus
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        leftInset: 0
        rightInset: 0
        topInset: 0
        bottomInset: 0

        background: Rectangle {
            color: capBtn.hovered
                ? (capBtn.isClose
                    ? "#c42b1c"
                    : Qt.rgba(capBtn.fg.r, capBtn.fg.g, capBtn.fg.b, capBtn.dark ? 0.12 : 0.10))
                : "transparent"
        }

        contentItem: Label {
            text: capBtn.sym
            font.family: "Segoe MDL2 Assets"
            font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: (capBtn.isClose && capBtn.hovered) ? "white" : capBtn.fg
        }
    }

    // The two left caption buttons (hamburger menu / view-toggle). Registered with
    // QWindowKit as hit-test-visible, which makes them HTCLIENT: when the cursor leaves
    // one into the content below, no client/non-client boundary is crossed, so Qt never
    // delivers a hover-leave and the built-in `hovered` sticks true. The custom flat
    // background gates the highlight on `!suppressHighlight` so the content hover (see
    // contentHover) can drop it. Hover tint matches CaptionButton; like it, the theme
    // colors are passed in (fg/dark) rather than read from outer ids.
    component TitleBarToolButton: ToolButton {
        id: tbBtn

        property bool suppressHighlight: false
        property color fg: "black"
        property bool dark: false

        Layout.preferredWidth: 40
        Layout.preferredHeight: 32
        Layout.alignment: Qt.AlignVCenter
        focusPolicy: Qt.NoFocus

        background: Rectangle {
            radius: 4
            color: tbBtn.down
                ? Qt.rgba(tbBtn.fg.r, tbBtn.fg.g, tbBtn.fg.b, tbBtn.dark ? 0.18 : 0.16)
                : (tbBtn.hovered && !tbBtn.suppressHighlight)
                    ? Qt.rgba(tbBtn.fg.r, tbBtn.fg.g, tbBtn.fg.b, tbBtn.dark ? 0.12 : 0.10)
                    : "transparent"
        }
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

    header: ToolBar {
        objectName: "titleBar"
        palette: root.palette

        background: Rectangle {
            color: theme.window

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.border
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            spacing: 0

            Image {
                objectName: "windowIcon"
                source: "qrc:/icons/hitsc-32.png"
                fillMode: Image.PreserveAspectFit
                mipmap: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                Layout.rightMargin: 6
            }

            TitleBarToolButton {
                id: menuButton
                objectName: "menuButton"

                palette: root.palette
                fg: theme.text
                dark: theme.darkMode
                suppressHighlight: contentHover.hovered

                onClicked: appMenu.popup(menuButton, 0, menuButton.height + 4)

                contentItem: Item {
                    Column {
                        anchors.centerIn: parent
                        spacing: 4

                        Repeater {
                            model: 3

                            delegate: Rectangle {
                                width: 18
                                height: 2
                                radius: 1
                                color: theme.text
                            }
                        }
                    }
                }
            }

            TitleBarToolButton {
                id: viewToggleButton
                objectName: "viewToggleButton"

                Layout.rightMargin: 6
                palette: root.palette
                fg: theme.text
                dark: theme.darkMode
                suppressHighlight: contentHover.hovered

                onClicked: windowPlacement.setMode(root.viewMode === "mini" ? "expanded" : "mini")

                contentItem: Item {
                    Grid {
                        anchors.centerIn: parent
                        columns: 2
                        rowSpacing: 3
                        columnSpacing: 3
                        visible: root.viewMode === "mini"

                        Repeater {
                            model: 4

                            delegate: Rectangle {
                                width: 7
                                height: 7
                                radius: 1.5
                                color: theme.text
                            }
                        }
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        visible: root.viewMode !== "mini"
                        width: 18
                        height: 13
                        radius: 2
                        color: "transparent"
                        border.color: theme.text
                        border.width: 2
                    }
                }
            }

            Label {
                text: root.title
                color: theme.text
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 13
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
            }

            Item {
                Layout.fillWidth: true
            }

            CaptionButton {
                objectName: "minButton"
                sym: String.fromCharCode(0xE921)
                fg: theme.text
                dark: theme.darkMode
                onClicked: root.showMinimized()
            }

            CaptionButton {
                objectName: "maxButton"
                visible: root.viewMode !== "mini"
                sym: root.visibility === Window.Maximized ? String.fromCharCode(0xE923) : String.fromCharCode(0xE922)
                fg: theme.text
                dark: theme.darkMode
                onClicked: root.toggleMaximized()
            }

            CaptionButton {
                objectName: "closeButton"
                sym: String.fromCharCode(0xE8BB)
                isClose: true
                fg: theme.text
                dark: theme.darkMode
                onClicked: root.close()
            }
        }

        Menu {
            id: appMenu

            palette: root.palette

            MenuItem {
                text: "Add Host…"
                palette: root.palette
                onTriggered: addHostDialog.openFresh()
            }

            MenuSeparator {}

            MenuItem {
                text: "About"
                palette: root.palette
                onTriggered: aboutDialog.open()
            }

            MenuItem {
                text: "Exit"
                palette: root.palette
                onTriggered: Qt.quit()
            }
        }
    }

    // Authoritative "pointer is in the client area" signal for the title-bar buttons
    // above. HoverHandler is passive -- it never grabs or blocks, so it reports hover for
    // the whole content region (below the caption) without stealing events from the
    // tiles/panel drawn on top. The hit-test-visible caption buttons gate their highlight
    // on this so it drops the instant the cursor enters the content (the one transition
    // QWindowKit doesn't produce a leave for).
    Item {
        anchors.fill: parent

        HoverHandler {
            id: contentHover
        }
    }

    Item {
        id: expandedView

        anchors.fill: parent
        visible: root.viewMode !== "mini"

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
            property int expandedTileHeight: 154
            property int compactTileHeight: 68
            property int extraCompactTileHeight: 44
            property int tileCount: hostModel.count
            property int expandedRows: Math.max(1, Math.ceil(tileCount / columnCount))
            property int expandedContentHeight: expandedRows * expandedTileHeight + Math.max(0, expandedRows - 1) * rowSpacing
            property int compactContentHeight: expandedRows * compactTileHeight + Math.max(0, expandedRows - 1) * rowSpacing
            property int layoutMode: expandedContentHeight <= hostScroll.availableHeight - 8 ? 0 : compactContentHeight <= hostScroll.availableHeight - 8 ? 1 : 2
            property bool compactMode: layoutMode === 1
            property bool extraCompactMode: layoutMode === 2
            property int tileHeight: extraCompactMode ? extraCompactTileHeight : compactMode ? compactTileHeight : expandedTileHeight
            property int deleteDelayMs: 5000
            property int currentIndex: -1

            function selectIndex(i) {
                if (i < 0 || i >= tileCount)
                    return
                currentIndex = i
                const item = hostRepeater.itemAt(i)
                if (item)
                    item.forceActiveFocus()
            }

            function moveSelection(fromIndex, key) {
                let target = fromIndex
                if (key === Qt.Key_Right)
                    target = fromIndex + 1
                else if (key === Qt.Key_Left)
                    target = fromIndex - 1
                else if (key === Qt.Key_Down)
                    target = fromIndex + columnCount
                else if (key === Qt.Key_Up)
                    target = fromIndex - columnCount
                else
                    return false
                if (target >= 0 && target < tileCount)
                    selectIndex(target)
                return true
            }

            onTileCountChanged: if (currentIndex >= tileCount)
                currentIndex = tileCount - 1

            width: hostScroll.availableWidth
            columns: columnCount
            rowSpacing: 14
            columnSpacing: 14

            Repeater {
                id: hostRepeater

                model: hostModel

                delegate: Item {
                    id: hostTile

                    required property string hostId
                    required property string type
                    required property string typeLabel
                    required property string url
                    required property string host
                    required property string status
                    required property string statusLabel
                    required property int index

                    property bool pendingDelete: false
                    property real deleteProgress: 0

                    Layout.preferredWidth: hostGrid.tileWidth
                    Layout.preferredHeight: hostGrid.tileHeight
                    activeFocusOnTab: true

                    readonly property bool selected: hostGrid.currentIndex === index

                    onActiveFocusChanged: if (activeFocus)
                        hostGrid.currentIndex = index

                    function beginPendingDelete() {
                        forceActiveFocus()
                        pendingDelete = true
                        deleteProgress = 0
                        deleteTimer.restart()
                        deleteWipe.restart()
                    }

                    function cancelPendingDelete() {
                        if (!pendingDelete)
                            return false
                        deleteTimer.stop()
                        deleteWipe.stop()
                        pendingDelete = false
                        deleteProgress = 0
                        return true
                    }

                    function confirmPendingDelete() {
                        deleteTimer.stop()
                        deleteWipe.stop()
                        pendingDelete = false
                        deleteProgress = 0
                        const result = hostModel.deleteHost(hostId)
                        if (!result.ok)
                            console.error(result.error)
                    }

                    function activateConnect() {
                        if (cancelPendingDelete())
                            return
                        root.connectHost(hostId)
                    }

                    function activateEdit() {
                        if (cancelPendingDelete())
                            return
                        addHostDialog.openForEdit(hostId)
                    }

                    Keys.onReturnPressed: Qt.callLater(hostTile.activateConnect)
                    Keys.onEnterPressed: Qt.callLater(hostTile.activateConnect)
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_F2) {
                            Qt.callLater(hostTile.activateEdit)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Delete) {
                            if (pendingDelete)
                                confirmPendingDelete()
                            else
                                beginPendingDelete()
                            event.accepted = true
                        } else if (hostGrid.moveSelection(hostTile.index, event.key)) {
                            event.accepted = true
                        }
                    }

                    Timer {
                        id: deleteTimer

                        interval: hostGrid.deleteDelayMs
                        repeat: false
                        onTriggered: hostTile.confirmPendingDelete()
                    }

                    NumberAnimation {
                        id: deleteWipe

                        target: hostTile
                        property: "deleteProgress"
                        from: 0
                        to: 1
                        duration: hostGrid.deleteDelayMs
                        easing.type: Easing.Linear
                    }

                    Rectangle {
                        id: hostCard

                        anchors.fill: parent
                        radius: 8
                        color: hostHover.hovered || hostTile.selected ? theme.panelHover : theme.panel
                        border.color: hostHover.hovered || hostTile.selected ? theme.borderHover : theme.border
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
                                hostGrid.selectIndex(hostTile.index)
                                if (hostTile.cancelPendingDelete())
                                    return
                                const px = eventPoint.position.x
                                const py = eventPoint.position.y
                                Qt.callLater(() => hostMenu.popup(hostCard, px, py))
                            }
                        }

                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onSingleTapped: {
                                hostGrid.selectIndex(hostTile.index)
                                hostTile.cancelPendingDelete()
                            }
                            onDoubleTapped: {
                                hostGrid.selectIndex(hostTile.index)
                                Qt.callLater(hostTile.activateConnect)
                            }
                        }

                        Menu {
                            id: hostMenu

                            palette: root.palette

                            MenuItem {
                                text: "Connect"
                                font.bold: true
                                palette: root.palette
                                onTriggered: hostTile.activateConnect()
                            }

                            MenuItem {
                                text: "Edit"
                                palette: root.palette
                                onTriggered: hostTile.activateEdit()
                            }

                            MenuItem {
                                text: "BMC cold reset"
                                visible: hostTile.type === "auto" || hostTile.type === "megarac"
                                         || hostTile.type === "aten"
                                height: visible ? implicitHeight : 0
                                palette: root.palette
                                onTriggered: root.coldResetHost(hostTile.hostId)
                            }

                            MenuItem {
                                text: "Delete"
                                palette: root.palette
                                onTriggered: hostTile.beginPendingDelete()
                            }
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: hostGrid.extraCompactMode ? 10 : hostGrid.compactMode ? 12 : 16
                            spacing: hostGrid.extraCompactMode ? 0 : hostGrid.compactMode ? 4 : 8

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: hostGrid.extraCompactMode ? 8 : hostGrid.compactMode ? 8 : 10

                                Label {
                                    text: host
                                    color: theme.text
                                    font.pixelSize: hostGrid.extraCompactMode ? 13 : hostGrid.compactMode ? 18 : 20
                                    font.weight: hostGrid.extraCompactMode ? Font.Normal : Font.DemiBold
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

                            Item {
                                visible: !hostGrid.compactMode && !hostGrid.extraCompactMode
                                Layout.fillHeight: true
                            }

                            RowLayout {
                                visible: !hostGrid.compactMode && !hostGrid.extraCompactMode
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

                        Item {
                            anchors.fill: parent
                            visible: hostTile.pendingDelete
                            clip: true

                            readonly property real fadeWidth: 16

                            Rectangle {
                                id: deleteMask

                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                width: parent.width * hostTile.deleteProgress
                                color: theme.window
                                opacity: 0.92
                            }

                            Rectangle {
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                x: Math.max(0, parent.width - deleteMask.width - width)
                                width: Math.min(parent.width - deleteMask.width, parent.fadeWidth)
                                visible: deleteMask.width > 0 && deleteMask.width < parent.width

                                gradient: Gradient {
                                    orientation: Gradient.Horizontal

                                    GradientStop {
                                        position: 0
                                        color: Qt.rgba(theme.window.r, theme.window.g, theme.window.b, 0)
                                    }

                                    GradientStop {
                                        position: 1
                                        color: Qt.rgba(theme.window.r, theme.window.g, theme.window.b, 0.92)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        id: emptyState

        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 380)
        visible: hostModel.count === 0
        spacing: 8

        Label {
            text: "↖"
            color: theme.mutedText
            font.pixelSize: 40
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: "No hosts yet"
            color: theme.text
            font.pixelSize: 22
            font.weight: Font.DemiBold
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: "Open the menu in the top-left corner to add your first host."
            color: theme.mutedText
            font.pixelSize: 14
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
    }

    MiniLauncherPanel {
        id: miniView

        anchors.fill: parent
        visible: root.viewMode === "mini"

        theme: theme
        controlPalette: root.palette
        statusColor: root.statusColor
    }

    Dialog {
        id: aboutDialog

        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: Math.min(root.width - 80, 360)
        padding: 24
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)

        background: Rectangle {
            radius: 8
            color: theme.window
            border.color: theme.border
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 12

            Image {
                source: "qrc:/icons/hitsc-128.png"
                sourceSize.width: 72
                sourceSize.height: 72
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: "hitsc"
                color: theme.text
                font.pixelSize: 26
                font.weight: Font.DemiBold
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: Qt.application.version.length > 0 ? "Version " + Qt.application.version : ""
                visible: text.length > 0
                color: theme.mutedText
                font.pixelSize: 12
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: "HTTPS IPMI Terminal Services Client"
                color: theme.mutedText
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            Button {
                text: "Close"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                palette: root.palette
                onClicked: aboutDialog.close()
            }
        }
    }

    Dialog {
        id: addHostDialog

        property bool editing: false
        property string editingHostId: ""
        property string errorText: ""
        readonly property string passwordMismatchText: "Passwords do not match."

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
            hostField.text = ""
            usernameField.text = ""
            passwordField.text = ""
            repeatPasswordField.text = ""
            errorText = ""
            open()
            hostField.forceActiveFocus()
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
            hostField.text = host.host
            usernameField.text = host.username
            passwordField.text = ""
            repeatPasswordField.text = ""
            errorText = ""
            open()
            hostField.forceActiveFocus()
        }

        function validatePasswordMatch() {
            if (repeatPasswordField.text.length > 0 && passwordField.text !== repeatPasswordField.text) {
                errorText = passwordMismatchText
                return false
            }

            if (errorText === passwordMismatchText)
                errorText = ""

            return true
        }

        function submit() {
            if (!validatePasswordMatch())
                return

            const typeKey = typeCombo.currentValue || typeCombo.currentText
            const result = addHostDialog.editing
                ? hostModel.updateHost(
                      addHostDialog.editingHostId,
                      typeKey,
                      hostField.text,
                      usernameField.text,
                      passwordField.text,
                      repeatPasswordField.text)
                : hostModel.addHost(
                      typeKey,
                      hostField.text,
                      usernameField.text,
                      passwordField.text,
                      repeatPasswordField.text)
            if (result.ok) {
                addHostDialog.close()
            } else {
                addHostDialog.errorText = result.error
            }
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
                    Layout.preferredWidth: 132
                    Layout.minimumWidth: 120
                    palette: root.palette
                }

                TextField {
                    id: hostField
                    placeholderText: "host or host:port"
                    Layout.fillWidth: true
                    palette: root.palette
                    inputMethodHints: Qt.ImhUrlCharactersOnly
                    onAccepted: addHostDialog.submit()
                }
            }

            TextField {
                id: usernameField
                placeholderText: "Username"
                Layout.fillWidth: true
                palette: root.palette
                onAccepted: addHostDialog.submit()
            }

            TextField {
                id: passwordField
                placeholderText: addHostDialog.editing ? "New password" : "Password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                palette: root.palette
                onTextEdited: addHostDialog.validatePasswordMatch()
                onAccepted: addHostDialog.submit()
            }

            TextField {
                id: repeatPasswordField
                placeholderText: addHostDialog.editing ? "Repeat new password" : "Repeat password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                palette: root.palette
                onTextEdited: addHostDialog.validatePasswordMatch()
                onAccepted: addHostDialog.submit()
            }

            Label {
                text: addHostDialog.errorText
                opacity: text.length > 0 ? 1 : 0
                color: "#d75f5f"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.preferredHeight: 20
                clip: true
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
                    onClicked: addHostDialog.submit()
                }
            }
        }
    }
}
