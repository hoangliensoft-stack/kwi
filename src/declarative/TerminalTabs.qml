// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kwi

RowLayout {
    id: root

    required property SessionModel sessions
    property WindowManager dragController: null
    property int position: Controls.TabBar.Header
    property bool showStatusIcons: true
    readonly property int count: tabBar.count
    readonly property int currentIndex: sessions.currentIndex
    readonly property ListView view: tabBar.contentItem as ListView
    readonly property bool overflowing: count * Kirigami.Units.gridUnit * 8 > width

    // originX remains the content's left edge in right-to-left layouts.
    readonly property real occupiedLeft: view.originX - view.contentX
    readonly property real occupiedRight: occupiedLeft + view.contentWidth

    signal activated(int index)
    signal contextRequested(TerminalSession session)
    signal renameRequested(int index)
    signal newTabRequested

    objectName: "terminalTabs"
    spacing: 0

    onEnabledChanged: {
        if (!enabled && dragHelper.local)
            dragHelper.finish(Qt.IgnoreAction);
    }

    // The drag must survive removal of its source tab delegate.
    Item {
        id: dragHelper

        objectName: "terminalTabDrag"
        visible: false

        property string token: ""
        property var preview: null
        property TerminalTabButton tab: null
        property point hotSpot
        property point pointerPosition
        property bool exporting: false
        readonly property bool local: tab !== null && !exporting
        readonly property real tabX: tab ? Math.max(0, Math.min(root.view.width - tab.width, pointerPosition.x - hotSpot.x)) : 0

        onTabChanged: {
            if (!tab && token.length > 0 && !exporting)
                finish(Qt.IgnoreAction);
        }

        Drag.source: dragHelper
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.MoveAction
        Drag.proposedAction: Qt.MoveAction
        Drag.mimeData: ({ "application/x-kwi-terminal-tab": token })

        Drag.onDragFinished: action => finish(action)

        function finish(action: int): void {
            root.dragController.finishTabDrag(token, action);
            token = "";
            tab = null;
            exporting = false;
            dragHelper.Drag.imageSource = "";
            preview = null;
        }

        function start(button: Item, session: TerminalSession, hotSpot: point, scenePosition: point): void {
            const token = root.dragController.beginTabDrag(root.sessions, session);

            if (token.length === 0)
                return;

            dragHelper.token = token;
            dragHelper.hotSpot = hotSpot;
            tab = button;
            move(scenePosition);
        }

        function release(button: Item, accepted: bool): void {
            if (tab !== button || !local)
                return;

            const dropped = accepted && root.dragController.dropTab(token, root.sessions, tabDrop.insertionIndex);
            finish(dropped ? Qt.MoveAction : Qt.IgnoreAction);
        }

        function move(scenePosition: point): void {
            if (!local)
                return;

            pointerPosition = root.view.mapFromItem(null, scenePosition);
            const centerX = pointerPosition.x - hotSpot.x + tab.width / 2;
            const centerY = pointerPosition.y - hotSpot.y + tab.height / 2;
            const margin = 2 * tab.height;

            if (centerX >= 0 && centerX <= root.view.width && centerY >= -margin && centerY <= root.view.height + margin)
                return;

            exporting = true;
            tab.grabToImage(result => {
                if (!dragHelper.tab || !dragHelper.tab.dragGestureActive || !root.enabled) {
                    dragHelper.finish(Qt.IgnoreAction);
                    return;
                }

                dragHelper.preview = result;
                dragHelper.Drag.imageSource = result.url;
                dragHelper.Drag.hotSpot = dragHelper.hotSpot;
                dragHelper.Drag.active = true;
            });
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: dragHelper.local
        onActivated: dragHelper.finish(Qt.IgnoreAction)
    }

    function scrollTabs(distance: real): void {
        view.contentX = Math.max(view.originX, Math.min(view.originX + Math.max(0, view.contentWidth - view.width), view.contentX + distance));
    }

    Controls.ToolButton {
        objectName: "previousTabsButton"
        Kirigami.MnemonicData.enabled: false
        visible: root.overflowing
        enabled: !root.view.atXBeginning
        icon.name: "arrow-left"
        text: KI18n.i18nc("@action:button", "Scroll Tabs Left")
        display: Controls.AbstractButton.IconOnly

        onClicked: root.scrollTabs(-root.view.width / 2)

        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    Controls.TabBar {
        id: tabBar

        objectName: "terminalTabBar"
        // The enclosing toolbar owns the surface behind tabs and buttons.
        background: null
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        position: root.position
        currentIndex: root.sessions.currentIndex
        contentItem.objectName: "terminalTabList"

        Binding {
            root.view.snapMode: ListView.NoSnap
        }

        // Guard the desktop style's height calculation while the Repeater is empty.
        contentItem.implicitHeight: tabBar.count > 0 && tabBar.itemAt(0) ? tabBar.itemAt(0).implicitHeight : 0

        // Fill the toolbar height without making implicitHeight depend on actual height.
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, contentItem.implicitHeight + topPadding + bottomPadding)
        contentHeight: availableHeight

        readonly property real tabWidth: Math.max(Kirigami.Units.gridUnit * 8, Math.min(Kirigami.Units.gridUnit * 10, width / Math.max(1, count)))

        function revealCurrent(): void {
            setCurrentIndex(root.sessions.currentIndex);

            if (currentIndex >= 0)
                root.view.positionViewAtIndex(currentIndex, ListView.Contain);
        }

        function navigateTo(index: int): void {
            root.activated(index);
            const button = itemAt(index);

            if (button)
                button.forceActiveFocus(Qt.TabFocusReason);
        }

        // Ignore intermediate container indexes during model changes.
        onCurrentIndexChanged: Qt.callLater(revealCurrent)
        onCountChanged: Qt.callLater(revealCurrent)
        onWidthChanged: Qt.callLater(revealCurrent)

        Connections {
            target: root.sessions

            function onCurrentIndexChanged(): void {
                tabBar.revealCurrent();
            }
        }

        function navigateKey(key: int): bool {
            switch (key) {
            case Qt.Key_Left:
                navigateTo(Math.max(0, root.currentIndex - 1));
                break;
            case Qt.Key_Right:
                navigateTo(Math.min(root.count - 1, root.currentIndex + 1));
                break;
            case Qt.Key_Home:
                navigateTo(0);
                break;
            case Qt.Key_End:
                navigateTo(root.count - 1);
                break;
            default:
                return false;
            }

            return true;
        }

        Keys.onPressed: event => event.accepted = navigateKey(event.key)

        Repeater {
            model: root.sessions

            delegate: TerminalTabButton {
                id: tabButton

                width: tabBar.tabWidth
                height: tabBar.availableHeight
                showStatusIcons: root.showStatusIcons
                draggable: root.dragController !== null
                dragging: dragHelper.Drag.active && dragHelper.tab === tabButton
                readonly property bool moving: dragHelper.local && dragHelper.tab === tabButton
                z: moving ? 1 : 0

                transform: Translate {
                    x: tabButton.moving ? dragHelper.tabX - tabButton.x + root.view.contentX : 0
                }

                // Carry the toolbar surface behind the moving tab's styled background.
                Rectangle {
                    anchors.fill: parent
                    z: -2
                    visible: tabButton.moving
                    color: Kirigami.Theme.backgroundColor
                }

                onClicked: root.activated(index)
                onCloseRequested: root.sessions.closeSession(index)
                onContextRequested: root.contextRequested(session)
                onRenameRequested: root.renameRequested(index)
                onDragRequested: (hotSpot, scenePosition) => dragHelper.start(tabButton, session, hotSpot, scenePosition)
                onDragMoved: scenePosition => dragHelper.move(scenePosition)
                onDragReleased: accepted => dragHelper.release(tabButton, accepted)
                onNavigationRequested: key => tabBar.navigateKey(key)
            }
        }

        // Accent gradient behind the current tab, fading from left to right. It lives
        // in the list's content so it scrolls with the tabs, and slides when the
        // current tab changes.
        Rectangle {
            id: currentIndicator

            objectName: "terminalTabIndicator"
            parent: root.view.contentItem
            // Above the list's highlight, below the tab delegates.
            z: 0.5

            readonly property Item tab: tabBar.currentItem
            readonly property bool following: dragHelper.local && dragHelper.tab === tab
            readonly property color accent: Kirigami.Theme.highlightColor

            visible: tab !== null
            x: !tab ? 0 : following ? dragHelper.tabX + root.view.contentX : tab.x
            y: tab ? tab.y + tab.topInset : 0
            width: tab ? tab.width : 0
            height: tab ? Math.max(0, tab.height - tab.topInset - tab.bottomInset) : 0
            radius: Kirigami.Units.cornerRadius

            gradient: Gradient {
                orientation: Gradient.Horizontal

                GradientStop {
                    position: 0
                    color: Qt.rgba(currentIndicator.accent.r, currentIndicator.accent.g, currentIndicator.accent.b, 0.32)
                }

                GradientStop {
                    position: 0.55
                    color: Qt.rgba(currentIndicator.accent.r, currentIndicator.accent.g, currentIndicator.accent.b, 0.14)
                }

                GradientStop {
                    position: 1
                    color: Qt.rgba(currentIndicator.accent.r, currentIndicator.accent.g, currentIndicator.accent.b, 0.03)
                }
            }

            Behavior on x {
                enabled: !currentIndicator.following

                NumberAnimation {
                    duration: Kirigami.Units.longDuration
                    easing.type: Easing.OutCubic
                }
            }

            Behavior on width {
                NumberAnimation {
                    duration: Kirigami.Units.longDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        MouseArea {
            parent: root.view
            anchors.fill: parent
            z: 1
            acceptedButtons: Qt.NoButton

            property point accumulatedAngleDelta

            onWheel: event => {
                accumulatedAngleDelta = Qt.point(accumulatedAngleDelta.x + event.angleDelta.x, accumulatedAngleDelta.y + event.angleDelta.y);
                const direction = accumulatedAngleDelta.x >= 120 || accumulatedAngleDelta.y >= 120 ? -1
                    : accumulatedAngleDelta.x <= -120 || accumulatedAngleDelta.y <= -120 ? 1 : 0;

                if (direction !== 0) {
                    accumulatedAngleDelta = Qt.point(0, 0);
                    const index = Math.max(0, Math.min(root.count - 1, root.currentIndex + direction));

                    if (index !== root.currentIndex)
                        root.activated(index);
                }
            }
        }

        MouseArea {
            parent: root.view
            width: Math.max(0, Math.min(root.view.width, root.occupiedLeft))
            height: root.view.height
            acceptedButtons: Qt.LeftButton

            onDoubleClicked: root.newTabRequested()
        }

        DropArea {
            id: tabDrop

            objectName: "terminalTabDropArea"

            // An explicit parent keeps this overlay out of the TabBar's content model.
            parent: root.view
            anchors.fill: parent
            z: 2
            keys: ["application/x-kwi-terminal-tab"]

            readonly property real pointerX: dragHelper.local ? dragHelper.pointerPosition.x : drag.x
            readonly property bool reordering: containsDrag || dragHelper.local
            readonly property int insertionIndex: insertionAt(pointerX)

            function insertionAt(position: real): int {
                for (let index = 0; index < root.count; ++index) {
                    const tab = tabBar.itemAt(index);
                    const midpoint = tab.x + tab.width / 2 - root.view.contentX;

                    if (tabBar.mirrored ? position > midpoint : position < midpoint)
                        return index;
                }

                return root.count;
            }

            readonly property real insertionX: {
                const tab = tabBar.itemAt(Math.min(insertionIndex, root.count - 1));

                if (!tab)
                    return 0;

                const trailing = insertionIndex === root.count;

                return tab.x - root.view.contentX + ((trailing !== tabBar.mirrored) ? tab.width : 0);
            }

            readonly property int scrollDirection: !reordering || !root.overflowing ? 0 : pointerX < Kirigami.Units.gridUnit ? -1 : pointerX > width - Kirigami.Units.gridUnit ? 1 : 0

            onEntered: event => {
                event.accepted = root.dragController !== null && root.dragController.acceptsTabDrag(event.getDataAsString("application/x-kwi-terminal-tab"));
            }

            onDropped: event => {
                event.accepted = false;

                if (root.dragController !== null && root.dragController.dropTab(event.getDataAsString("application/x-kwi-terminal-tab"), root.sessions, insertionAt(event.x)))
                    event.accept(Qt.MoveAction);
            }

            Rectangle {
                objectName: "terminalTabDropIndicator"
                visible: tabDrop.reordering
                x: Math.max(0, Math.min(tabDrop.width - width, tabDrop.insertionX - width / 2))
                y: Kirigami.Units.smallSpacing
                width: Kirigami.Units.smallSpacing / 2
                height: Math.max(0, parent.height - 2 * y)
                color: Kirigami.Theme.highlightColor
            }

            Timer {
                interval: 60
                repeat: true
                running: tabDrop.scrollDirection !== 0

                onTriggered: root.scrollTabs(tabDrop.scrollDirection * Kirigami.Units.gridUnit)
            }
        }

        MouseArea {
            parent: root.view
            x: Math.max(0, Math.min(root.view.width, root.occupiedRight))
            width: root.view.width - x
            height: root.view.height
            acceptedButtons: Qt.LeftButton

            onDoubleClicked: root.newTabRequested()
        }
    }

    Controls.ToolButton {
        objectName: "nextTabsButton"
        Kirigami.MnemonicData.enabled: false
        visible: root.overflowing
        enabled: !root.view.atXEnd
        icon.name: "arrow-right"
        text: KI18n.i18nc("@action:button", "Scroll Tabs Right")
        display: Controls.AbstractButton.IconOnly

        onClicked: root.scrollTabs(root.view.width / 2)

        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    }
}
