// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtQuick.Window
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.ki18n
import org.kde.notification as KNotification
import org.kde.kirigami as Kirigami
import org.kde.kirigami.dialogs as KirigamiDialogs
import org.kde.kirigami.actioncollection as AC
import org.kde.kirigami.delegates as KirigamiDelegates
import org.kde.kirigamiaddons.settings as Settings
import org.kde.kwi

Kirigami.ApplicationWindow {
    id: root

    required property AppSettings appSettings
    required property PresetCatalog terminalPresets
    required property PresetEditor presetEditor
    required property AppShortcuts appShortcuts
    required property ColorSchemeModel terminalPalettes
    required property SessionModel sessionModel
    required property DesktopIntegration desktopIntegration

    objectName: "mainWindow"

    width: 1000
    height: 650
    minimumWidth: 480
    minimumHeight: 320
    visible: false

    // Keep the surface alpha-capable for runtime opacity changes.
    color: "transparent"

    background: Item {}

    // Kwi provides its own configurable Quit action.
    quitAction.enabled: false

    title: root.currentSession ? (root.appSettings.windowTitleFromApplication ? root.currentSession.applicationTitle : root.currentSession.title) : ""

    property WindowManager tabDragController: null
    property bool findVisible: false
    readonly property var terminal: terminalPane.terminal

    // Qt 6.11 Action shortcuts in unopened MenuItems can trigger through modal dialogs.
    // Remove when Qt blocks these shortcuts while a modal dialog is open.
    readonly property bool terminalActionsEnabled: !closeDialog.visible && !pasteDialog.visible && !aboutDialog.visible && !renameDialog.visible && !chooserDialog.visible && !aboutKdeDialog.visible && !saveDialog.visible && !settingsView.configViewItem
    readonly property TerminalSession currentSession: root.sessionModel.currentSession

    function launchPreset(name: string): void {
        if (root.sessionModel.newSessionWithPreset(name)) {
            root.focusTerminal();

        } else {
            root.showPassiveNotification(KI18n.i18nc("@info", "The terminal preset is no longer available."));
        }
    }

    component NewTabPresetMenu: Controls.Menu {
        id: presetMenu

        title: KI18n.i18nc("@title:menu", "New Tab with Preset")

        Instantiator {
            model: root.terminalPresets.entries

            delegate: Controls.MenuItem {
                required property var modelData

                objectName: "launchPreset_" + modelData.name
                text: modelData.label
                icon.name: "utilities-terminal"
                enabled: root.terminalActionsEnabled

                onTriggered: root.launchPreset(modelData.name)
            }

            onObjectAdded: (index, object) => presetMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => presetMenu.removeItem(object)
        }

        Controls.MenuSeparator {}

        Controls.MenuItem {
            text: KI18n.i18nc("@action:inmenu", "Manage Presets…")
            icon.name: "configure"

            onTriggered: settingsView.open("presets")
        }
    }

    function search(forward: bool): void {
        findBar.search(forward);
    }

    function showRename(session: TerminalSession): void {
        renameDialog.targetSession = session;
        renameField.text = session ? session.customTitle : "";
        renameDialog.open();
    }

    function showChooser(links: bool): void {
        chooserDialog.choosingLinks = links;
        chooserDialog.entries = links ? terminal.links() : root.sessionModel.entries();
        chooserDialog.open();
    }

    function chooseEntry(entry): void {
        chooserDialog.close();

        if (chooserDialog.choosingLinks) {
            root.desktopIntegration.openUrl(entry.url);
        } else {
            root.sessionModel.currentIndex = root.sessionModel.indexOf(entry.session);
        }
    }

    function initialWindowSize(): size {
        terminalToolBar.contentItem.ensurePolished();
        const terminalSize = terminalPane.sizeForGrid(root.appSettings.windowColumns, root.appSettings.windowRows);
        return Qt.size(terminalSize.width, terminalSize.height + terminalToolBar.implicitHeight);
    }

    function focusTerminal(): void {
        terminal.forceActiveFocus();
    }

    function showFind(): void {
        findVisible = true;
        findBar.focusSearch();
    }

    function hideFind(): void {
        findVisible = false;
        findBar.cancelPendingSearch();
        terminal.clearSearch();
        focusTerminal();
    }

    function switchTab(offset: int): void {
        if (root.sessionModel.count > 0) {
            root.sessionModel.currentIndex = (root.sessionModel.currentIndex + offset + root.sessionModel.count) % root.sessionModel.count;
            root.focusTerminal();
        }
    }

    onClosing: event => {
        event.accepted = false;
        root.sessionModel.closeAll();
    }

    Connections {
        target: root.sessionModel

        function onRowsAboutToBeRemoved(parent: var, first: int, last: int): void {
            if (last - first + 1 === root.sessionModel.count) {
                // Hide before removing the terminal's content from the scene.
                root.hide();
            }
        }

        function onCurrentIndexChanged(): void {
            if (pasteDialog.visible && pasteDialog.targetSession !== root.sessionModel.currentSession) {
                pasteDialog.reject();
            }
        }

        function onCloseConfirmationRequested(requestId: double, scope: int, title: string): void {
            if (pasteDialog.visible) {
                pasteDialog.reject();
            }

            if (renameDialog.visible) {
                renameDialog.reject();
            }

            if (chooserDialog.visible) {
                chooserDialog.reject();
            }

            closeDialog.requestId = requestId;
            closeDialog.scope = scope;
            closeDialog.sessionTitle = title;
            closeDialog.open();
        }

        function onCloseConfirmationCancelled(requestId: double): void {
            if (closeDialog.requestId === requestId) {
                closeDialog.requestId = 0;
                closeDialog.close();
            }
        }
    }

    Kirigami.Action {
        id: newWindowAction

        objectName: "newWindowAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("new_window")
        AC.ActionCollection.action: "new_window"
        enabled: root.terminalActionsEnabled

        onTriggered: root.sessionModel.newWindow()
    }

    Kirigami.Action {
        id: newTabAction

        objectName: "newTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("new_tab")
        AC.ActionCollection.action: "new_tab"
        enabled: root.terminalActionsEnabled

        onTriggered: {
            root.sessionModel.newSession();
            root.focusTerminal();
        }
    }

    Kirigami.Action {
        id: closeTabAction

        objectName: "closeTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("close_tab")
        AC.ActionCollection.action: "close_tab"
        enabled: root.terminalActionsEnabled && root.sessionModel.count > 0

        onTriggered: root.sessionModel.closeSession(root.sessionModel.currentIndex)
    }

    Kirigami.Action {
        id: renameTabAction

        objectName: "renameTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("rename_tab")
        AC.ActionCollection.action: "rename_tab"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.showRename(root.currentSession)
    }

    Kirigami.Action {
        id: nextTabAction

        objectName: "nextTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("next_tab")
        AC.ActionCollection.action: "next_tab"
        enabled: root.terminalActionsEnabled && root.sessionModel.count > 1

        onTriggered: root.switchTab(1)
    }

    Kirigami.Action {
        id: previousTabAction

        objectName: "previousTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("previous_tab")
        AC.ActionCollection.action: "previous_tab"
        enabled: root.terminalActionsEnabled && root.sessionModel.count > 1

        onTriggered: root.switchTab(-1)
    }

    Kirigami.Action {
        id: lastUsedTabAction

        objectName: "lastUsedTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("last_used_tab")
        AC.ActionCollection.action: "last_used_tab"
        enabled: root.terminalActionsEnabled && root.sessionModel.count > 1

        onTriggered: {
            root.sessionModel.activateLastUsed();
            root.focusTerminal();
        }
    }

    Kirigami.Action {
        id: moveTabLeftAction

        objectName: "moveTabLeftAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("move_tab_left")
        AC.ActionCollection.action: "move_tab_left"
        enabled: root.terminalActionsEnabled && root.sessionModel.currentIndex > 0

        onTriggered: root.sessionModel.moveSession(root.sessionModel.currentIndex, root.sessionModel.currentIndex - 1)
    }

    Kirigami.Action {
        id: moveTabRightAction

        objectName: "moveTabRightAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("move_tab_right")
        AC.ActionCollection.action: "move_tab_right"
        enabled: root.terminalActionsEnabled && root.sessionModel.currentIndex < root.sessionModel.count - 1

        onTriggered: root.sessionModel.moveSession(root.sessionModel.currentIndex, root.sessionModel.currentIndex + 1)
    }

    Kirigami.Action {
        id: detachTabAction

        objectName: "detachTabAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("detach_tab")
        AC.ActionCollection.action: "detach_tab"
        enabled: root.terminalActionsEnabled && root.sessionModel.count > 1

        onTriggered: root.sessionModel.detachSession(root.sessionModel.currentIndex)
    }

    Kirigami.Action {
        id: searchTabsAction

        objectName: "searchTabsAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("search_tabs")
        AC.ActionCollection.action: "search_tabs"
        enabled: root.terminalActionsEnabled

        onTriggered: root.showChooser(false)
    }

    Kirigami.Action {
        id: copyAction

        objectName: "copyAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("copy")
        AC.ActionCollection.action: "copy"
        enabled: root.terminalActionsEnabled && root.terminal.hasSelection

        onTriggered: root.terminal.copy()
    }

    Kirigami.Action {
        id: pasteAction

        objectName: "pasteAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("paste")
        AC.ActionCollection.action: "paste"
        enabled: root.terminalActionsEnabled && root.currentSession && root.currentSession.active && !root.currentSession.readOnly

        onTriggered: root.terminal.paste()
    }

    Kirigami.Action {
        id: pasteSelectionAction

        objectName: "pasteSelectionAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("paste_selection")
        AC.ActionCollection.action: "paste_selection"
        enabled: root.terminalActionsEnabled && root.currentSession && root.currentSession.active && !root.currentSession.readOnly

        onTriggered: root.terminal.pasteSelection()
    }

    Kirigami.Action {
        id: selectAllAction

        objectName: "selectAllAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("select_all")
        AC.ActionCollection.action: "select_all"
        enabled: root.terminalActionsEnabled

        onTriggered: root.terminal.selectAll()
    }

    Kirigami.Action {
        id: selectionModeAction

        objectName: "selectionModeAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("selection_mode")
        AC.ActionCollection.action: "selection_mode"
        enabled: root.terminalActionsEnabled
        checkable: true
        checked: root.terminal.keyboardSelectionMode

        onTriggered: {
            root.terminal.toggleSelectionMode();
            root.focusTerminal();
        }
    }

    Kirigami.Action {
        id: saveOutputAction

        objectName: "saveOutputAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("save_output")
        AC.ActionCollection.action: "save_output"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: {
            saveDialog.targetSession = root.currentSession;
            saveDialog.open();
        }
    }

    Kirigami.Action {
        id: clearHistoryAction

        objectName: "clearHistoryAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("clear_history")
        AC.ActionCollection.action: "clear_history"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.currentSession.clearScrollback()
    }

    Kirigami.Action {
        id: resetTerminalAction

        objectName: "resetTerminalAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("reset_terminal")
        AC.ActionCollection.action: "reset_terminal"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.currentSession.resetTerminal()
    }

    Kirigami.Action {
        id: findAction

        objectName: "findAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("find")
        AC.ActionCollection.action: "find"
        enabled: root.terminalActionsEnabled

        onTriggered: root.showFind()
    }

    Kirigami.Action {
        id: findNextAction

        objectName: "findNextAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("edit_find_next")
        AC.ActionCollection.action: "edit_find_next"
        enabled: root.terminalActionsEnabled && root.findVisible && findBar.text.length > 0

        onTriggered: root.search(true)
    }

    Kirigami.Action {
        id: findPreviousAction

        objectName: "findPreviousAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("edit_find_prev")
        AC.ActionCollection.action: "edit_find_prev"
        enabled: root.terminalActionsEnabled && root.findVisible && findBar.text.length > 0

        onTriggered: root.search(false)
    }

    Kirigami.Action {
        id: previousPromptAction

        objectName: "previousPromptAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("previous_prompt")
        AC.ActionCollection.action: "previous_prompt"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.currentSession.scrollPrompt(-1)
    }

    Kirigami.Action {
        id: nextPromptAction

        objectName: "nextPromptAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("next_prompt")
        AC.ActionCollection.action: "next_prompt"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.currentSession.scrollPrompt(1)
    }

    Kirigami.Action {
        id: copyCommandOutputAction

        objectName: "copyCommandOutputAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("copy_command_output")
        AC.ActionCollection.action: "copy_command_output"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: {
            if (!root.terminal.copyCommandOutput())
                root.showPassiveNotification(KI18n.i18nc("@info", "No command output is marked by the shell in retained history."));
        }
    }

    Kirigami.Action {
        id: openDirectoryAction

        objectName: "openDirectoryAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("open_directory")
        AC.ActionCollection.action: "open_directory"
        enabled: root.terminalActionsEnabled && root.currentSession

        onTriggered: root.desktopIntegration.openCurrentDirectory(root.currentSession)
    }

    Kirigami.Action {
        id: openLinkChooserAction

        objectName: "openLinkChooserAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("open_link_chooser")
        AC.ActionCollection.action: "open_link_chooser"
        enabled: root.terminalActionsEnabled

        onTriggered: root.showChooser(true)
    }

    Kirigami.Action {
        id: readOnlyAction

        objectName: "readOnlyAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("read_only")
        AC.ActionCollection.action: "read_only"
        enabled: root.terminalActionsEnabled && root.currentSession
        checkable: true
        checked: root.currentSession ? root.currentSession.readOnly : false

        onTriggered: root.currentSession.readOnly = !root.currentSession.readOnly
    }

    Kirigami.Action {
        id: monitorActivityAction

        objectName: "monitorActivityAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("monitor_activity")
        AC.ActionCollection.action: "monitor_activity"
        enabled: root.terminalActionsEnabled && root.currentSession && root.currentSession.active
        checkable: true
        checked: root.currentSession ? root.currentSession.monitorActivity : false

        onTriggered: root.currentSession.monitorActivity = !root.currentSession.monitorActivity
    }

    Kirigami.Action {
        id: monitorSilenceAction

        objectName: "monitorSilenceAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("monitor_silence")
        AC.ActionCollection.action: "monitor_silence"
        enabled: root.terminalActionsEnabled && root.currentSession && root.currentSession.active
        checkable: true
        checked: root.currentSession ? root.currentSession.monitorSilence : false

        onTriggered: root.currentSession.monitorSilence = !root.currentSession.monitorSilence
    }

    Kirigami.Action {
        id: fullscreenAction

        objectName: "fullscreenAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("fullscreen")
        AC.ActionCollection.action: "fullscreen"
        enabled: root.terminalActionsEnabled
        checkable: true
        checked: root.visibility === Window.FullScreen

        onTriggered: root.desktopIntegration.toggleFullScreen()
    }

    Kirigami.Action {
        id: zoomInAction

        objectName: "zoomInAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("view_zoom_in")
        AC.ActionCollection.action: "view_zoom_in"
        enabled: root.terminalActionsEnabled

        onTriggered: root.terminal.zoomIn()
    }

    Kirigami.Action {
        id: zoomOutAction

        objectName: "zoomOutAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("view_zoom_out")
        AC.ActionCollection.action: "view_zoom_out"
        enabled: root.terminalActionsEnabled

        onTriggered: root.terminal.zoomOut()
    }

    Kirigami.Action {
        id: resetZoomAction

        objectName: "resetZoomAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("reset_zoom")
        AC.ActionCollection.action: "reset_zoom"
        enabled: root.terminalActionsEnabled

        onTriggered: root.terminal.resetZoom()
    }

    Kirigami.Action {
        id: settingsAction

        objectName: "settingsAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("settings")
        AC.ActionCollection.action: "settings"
        enabled: root.terminalActionsEnabled

        onTriggered: settingsView.open()
    }

    Kirigami.Action {
        id: notificationsAction

        objectName: "notificationsAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("options_configure_notifications")
        AC.ActionCollection.action: "options_configure_notifications"
        // Remove these suppressions when KNotifications adds qmltypes for this singleton.
        enabled: root.terminalActionsEnabled && KNotification.NotificationConfiguration.isAvailable // qmllint disable unresolved-type missing-property

        onTriggered: KNotification.NotificationConfiguration.show() // qmllint disable unresolved-type missing-property
    }

    Kirigami.Action {
        id: menuAction

        objectName: "menuAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("menu")
        AC.ActionCollection.action: "menu"
        enabled: root.terminalActionsEnabled

        onTriggered: applicationMenu.open()
    }

    Kirigami.Action {
        id: applicationQuitAction

        objectName: "quitAction"
        AC.ActionCollection.collection: root.appShortcuts.collectionFor("quit")
        AC.ActionCollection.action: "quit"
        enabled: root.terminalActionsEnabled

        onTriggered: root.sessionModel.closeApplication()
    }

    Instantiator {
        model: 9

        delegate: Kirigami.Action {
            required property int index

            AC.ActionCollection.collection: root.appShortcuts.collectionFor("tab_" + (index + 1))
            AC.ActionCollection.action: "tab_" + (index + 1)
            enabled: root.terminalActionsEnabled && root.sessionModel.count > 1 && index < root.sessionModel.count

            onTriggered: {
                root.sessionModel.currentIndex = index;
                root.focusTerminal();
            }
        }
    }

    Settings.ConfigurationView {
        id: settingsView

        objectName: "settingsView"
        window: root
        modules: [
            Settings.ConfigurationModule {
                moduleId: "appearance"
                text: KI18n.i18nc("@title", "Appearance")
                icon.name: "preferences-desktop-theme-global"
                page: () => Qt.createComponent("org.kde.kwi", "AppearanceSettings")
                initialProperties: () => ({ appSettings: root.appSettings, terminalPalettes: root.terminalPalettes })
            },

            Settings.ConfigurationModule {
                moduleId: "behavior"
                text: KI18n.i18nc("@title", "Behavior")
                icon.name: "preferences-system"
                page: () => Qt.createComponent("org.kde.kwi", "BehaviorSettings")
                initialProperties: () => ({ appSettings: root.appSettings })
            },

            Settings.ConfigurationModule {
                moduleId: "terminal"
                text: KI18n.i18nc("@title", "Terminal")
                icon.name: "utilities-terminal"
                page: () => Qt.createComponent("org.kde.kwi", "TerminalSettings")
                initialProperties: () => ({ appSettings: root.appSettings })
            },

            Settings.ConfigurationModule {
                moduleId: "presets"
                text: KI18n.i18nc("@title", "Presets")
                icon.name: "view-list-details"
                page: () => Qt.createComponent("org.kde.kwi", "PresetsSettings")
                initialProperties: () => ({ appSettings: root.appSettings, terminalPresets: root.terminalPresets, editor: root.presetEditor })
            },

            Settings.ConfigurationModule {
                moduleId: "keyboard"
                text: KI18n.i18nc("@title", "Keyboard")
                icon.name: "input-keyboard"
                page: () => Qt.createComponent("org.kde.kwi", "KeyboardSettings")
            }
        ]
    }

    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.None
    pageStack.layers.Accessible.ignored: true

    pageStack.initialPage: Kirigami.Page {
        id: terminalPage

        padding: 0
        Accessible.ignored: true

        // Append tab drops not handled by a child drop area.
        background: DropArea {
            objectName: "terminalWindowDropArea"
            enabled: root.terminalActionsEnabled && root.tabDragController !== null
            keys: ["application/x-kwi-terminal-tab"]

            onEntered: event => {
                event.accepted = root.tabDragController !== null
                    && root.tabDragController.acceptsTabDrag(event.getDataAsString("application/x-kwi-terminal-tab"))
                    && root.sessionModel.indexOf(root.tabDragController.draggedSession) < 0;
            }

            onDropped: event => {
                event.accepted = false;

                if (root.tabDragController !== null && root.tabDragController.dropTab(event.getDataAsString("application/x-kwi-terminal-tab"), root.sessionModel, root.sessionModel.count))
                    event.accept(Qt.MoveAction);
            }
        }

        title: KI18n.i18nc("@title", "Terminal")

        GridLayout {
            anchors.fill: parent
            columns: 1
            rowSpacing: 0
            columnSpacing: 0

            Controls.ToolBar {
                id: terminalToolBar

                objectName: "terminalToolBar"
                Layout.row: root.appSettings.tabBarAtBottom ? 1 : 0
                Layout.fillWidth: true
                position: root.appSettings.tabBarAtBottom ? Controls.ToolBar.Footer : Controls.ToolBar.Header
                padding: Kirigami.Units.smallSpacing
                topPadding: position === Controls.ToolBar.Footer ? 0 : padding
                bottomPadding: position === Controls.ToolBar.Header ? 0 : padding
                Kirigami.Theme.colorSet: position === Controls.ToolBar.Header ? Kirigami.Theme.Header : Kirigami.Theme.Window
                Kirigami.Theme.inherit: false

                // Blend the tab bar into the terminal: same surface, same text color.
                readonly property bool followsTerminal: root.terminal && root.terminal.backgroundColor.valid && root.terminal.foregroundColor.valid
                Kirigami.Theme.backgroundColor: followsTerminal ? root.terminal.backgroundColor : systemToolBarColors.Kirigami.Theme.backgroundColor
                Kirigami.Theme.textColor: followsTerminal ? root.terminal.foregroundColor : systemToolBarColors.Kirigami.Theme.textColor

                // The system's toolbar colors, used until the terminal reports its own.
                Item {
                    id: systemToolBarColors

                    visible: false
                    Kirigami.Theme.colorSet: terminalToolBar.position === Controls.ToolBar.Header ? Kirigami.Theme.Header : Kirigami.Theme.Window
                    Kirigami.Theme.inherit: false
                }

                background: Rectangle {
                    // Compact minimum; the tabs and tool buttons set the height above it.
                    implicitHeight: 34
                    color: {
                        const surface = Kirigami.Theme.backgroundColor;

                        return terminalToolBar.followsTerminal ? Qt.rgba(surface.r, surface.g, surface.b, root.appSettings.backgroundOpacity) : surface;
                    }

                    Behavior on color {
                        ColorAnimation {
                            duration: Kirigami.Units.longDuration
                        }
                    }
                }

                contentItem: RowLayout {
                    objectName: "terminalToolBarContents"
                    spacing: Kirigami.Units.smallSpacing

                    Controls.ToolButton {
                        action: newTabAction
                        objectName: "newTabButton"

                        // Terminal Alt keys are reserved for explicitly configured shortcuts.
                        Kirigami.MnemonicData.enabled: false
                        display: Controls.AbstractButton.IconOnly
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small
                        Controls.ToolTip.text: newTabAction.text + (newTabAction.shortcut ? " (" + newTabAction.shortcut + ")" : "")
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    Controls.ToolButton {
                        id: presetMenuButton

                        objectName: "newTabPresetButton"
                        focusPolicy: Qt.TabFocus
                        visible: root.terminalPresets.names.length > 1
                        enabled: root.terminalActionsEnabled
                        text: KI18n.i18nc("@action:button", "New Tab with Preset")
                        icon.name: "arrow-down"
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small
                        display: Controls.AbstractButton.IconOnly
                        Kirigami.MnemonicData.enabled: false

                        down: pressed || presetMenu.visible
                        onClicked: presetMenu.open()

                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: (hovered || activeFocus) && !presetMenu.visible
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        NewTabPresetMenu {
                            id: presetMenu

                            modal: true
                            dim: false

                            objectName: "newTabPresetMenu"
                            y: root.appSettings.tabBarAtBottom ? -height : presetMenuButton.height
                        }
                    }

                    TerminalTabs {
                        id: sessionTabs

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        enabled: root.terminalActionsEnabled
                        sessions: root.sessionModel
                        dragController: root.tabDragController
                        position: root.appSettings.tabBarAtBottom ? Controls.TabBar.Footer : Controls.TabBar.Header
                        showStatusIcons: root.appSettings.showTabStatusIcons

                        onNewTabRequested: newTabAction.trigger()
                        onActivated: index => {
                            root.sessionModel.currentIndex = index;
                            root.focusTerminal();
                        }

                        onContextRequested: session => {
                            tabMenu.targetSession = session;
                            tabMenu.popup();
                        }

                        onRenameRequested: index => {
                            root.sessionModel.currentIndex = index;
                            root.showRename(root.currentSession);
                        }
                    }

                    Controls.ToolButton {
                        id: menuButton

                        objectName: "applicationMenuButton"
                        focusPolicy: Qt.TabFocus
                        Kirigami.MnemonicData.enabled: false
                        icon.name: "application-menu"
                        text: KI18n.i18nc("@action:button", "Application Menu")
                        display: Controls.AbstractButton.IconOnly
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small

                        down: pressed || applicationMenu.visible
                        onClicked: applicationMenu.open()

                        Controls.ToolTip.text: menuAction.text + (menuAction.shortcut ? " (" + menuAction.shortcut + ")" : "")
                        Controls.ToolTip.visible: (hovered || activeFocus) && !applicationMenu.visible
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        Controls.Menu {
                            id: applicationMenu

                            objectName: "applicationMenu"
                            y: root.appSettings.tabBarAtBottom ? -height : menuButton.height

                            modal: true
                            dim: false

                            Controls.MenuItem {
                                action: newWindowAction
                            }

                            Controls.MenuItem {
                                action: newTabAction
                            }

                            NewTabPresetMenu {}

                            Controls.Menu {
                                title: KI18n.i18nc("@title:menu", "Tabs")

                                Controls.MenuItem {
                                    action: renameTabAction
                                }

                                Controls.MenuItem {
                                    action: nextTabAction
                                }

                                Controls.MenuItem {
                                    action: previousTabAction
                                }

                                Controls.MenuItem {
                                    action: lastUsedTabAction
                                }

                                Controls.MenuItem {
                                    action: searchTabsAction
                                }

                                Controls.MenuSeparator {}

                                Controls.MenuItem {
                                    action: moveTabLeftAction
                                }

                                Controls.MenuItem {
                                    action: moveTabRightAction
                                }

                                Controls.MenuItem {
                                    action: detachTabAction
                                }

                                Controls.MenuItem {
                                    action: closeTabAction
                                }
                            }

                            Controls.Menu {
                                title: KI18n.i18nc("@title:menu", "Edit")

                                Controls.MenuItem {
                                    action: copyAction
                                }

                                Controls.MenuItem {
                                    action: pasteAction
                                }

                                Controls.MenuItem {
                                    action: pasteSelectionAction
                                }

                                Controls.MenuItem {
                                    action: selectAllAction
                                }

                                Controls.MenuItem {
                                    action: selectionModeAction
                                }

                                Controls.MenuItem {
                                    action: readOnlyAction
                                }

                                Controls.MenuSeparator {}

                                Controls.MenuItem {
                                    action: findAction
                                }

                                Controls.MenuItem {
                                    action: findNextAction
                                }

                                Controls.MenuItem {
                                    action: findPreviousAction
                                }
                            }

                            Controls.Menu {
                                title: KI18n.i18nc("@title:menu", "Output")

                                Controls.MenuItem {
                                    action: saveOutputAction
                                }

                                Controls.MenuItem {
                                    action: clearHistoryAction
                                }

                                Controls.MenuItem {
                                    action: resetTerminalAction
                                }

                                Controls.MenuSeparator {}

                                Controls.MenuItem {
                                    action: previousPromptAction
                                }

                                Controls.MenuItem {
                                    action: nextPromptAction
                                }

                                Controls.MenuItem {
                                    action: copyCommandOutputAction
                                }

                                Controls.MenuItem {
                                    action: openDirectoryAction
                                }

                                Controls.MenuItem {
                                    action: openLinkChooserAction
                                }

                                Controls.MenuSeparator {}

                                Controls.MenuItem {
                                    action: monitorActivityAction
                                }

                                Controls.MenuItem {
                                    action: monitorSilenceAction
                                }
                            }

                            Controls.Menu {
                                title: KI18n.i18nc("@title:menu", "View")

                                Controls.MenuItem {
                                    action: zoomInAction
                                }

                                Controls.MenuItem {
                                    action: zoomOutAction
                                }

                                Controls.MenuItem {
                                    action: resetZoomAction
                                }

                                Controls.MenuItem {
                                    action: fullscreenAction
                                }
                            }

                            Controls.MenuSeparator {}

                            Controls.MenuItem {
                                action: settingsAction
                            }

                            Controls.MenuItem {
                                action: notificationsAction
                            }

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action", "About Kwi")
                                icon.name: "help-about"

                                onTriggered: aboutDialog.open()
                            }

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action", "About KDE")
                                icon.name: "kde"

                                onTriggered: aboutKdeDialog.open()
                            }

                            Controls.MenuSeparator {}

                            Controls.MenuItem {
                                action: applicationQuitAction
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.row: root.appSettings.tabBarAtBottom ? 0 : 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                TerminalSearchBar {
                    id: findBar

                    visible: root.findVisible
                    Layout.fillWidth: true
                    terminal: root.terminal
                    tabBarAtTop: !root.appSettings.tabBarAtBottom

                    onCloseRequested: root.hideFind()
                }

                Kirigami.InlineMessage {
                    objectName: "sessionErrorMessage"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    type: Kirigami.MessageType.Error
                    text: TextFormatting.plainTextToHtml(root.sessionModel.currentSession ? root.sessionModel.currentSession.errorString : "")
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: root.currentSession && root.currentSession.finished && !root.currentSession.errorString
                    type: root.currentSession && root.currentSession.exitCode !== 0 ? Kirigami.MessageType.Warning : Kirigami.MessageType.Information
                    text: root.currentSession && root.currentSession.exitCode !== 0 ? KI18n.i18nc("@info", "Process exited with status %1.", root.currentSession.exitCode) : KI18n.i18nc("@info", "Process finished.")
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: false
                    type: Kirigami.MessageType.Error
                    text: TextFormatting.plainTextToHtml(root.desktopIntegration.errorString)
                    showCloseButton: true

                    onTextChanged: visible = text.length > 0
                    onVisibleChanged: {
                        if (!visible && text.length > 0) {
                            root.desktopIntegration.clearError();
                        }
                    }
                }

                TerminalPane {
                    id: terminalPane

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    applicationSession: root.sessionModel.currentSession

                    // Don't let the window's Kirigami.PageRow shortcuts interfere with the terminal.
                    Keys.onShortcutOverride: event => {
                        event.accepted = event.matches(StandardKey.Back) || event.matches(StandardKey.Forward);
                    }

                    onMultilinePasteRequested: (origin, text) => {
                        pasteDialog.targetSession = origin;
                        pasteDialog.pasteContent = text;
                        pasteDialog.open();
                    }

                    onContextMenuRequested: position => {
                        terminalMenu.linkUrl = terminal.hoveredLink;
                        webShortcuts.refresh(terminal.selectedText);
                        terminalMenu.popup(terminal, position.x, position.y);
                    }

                    onLinkActivated: url => root.desktopIntegration.openUrl(url)
                    Component.onCompleted: Qt.callLater(root.focusTerminal)

                    Controls.Menu {
                        id: terminalMenu

                        objectName: "terminalContextMenu"
                        modal: true
                        dim: false
                        property url linkUrl

                        WebShortcutModel {
                            id: webShortcuts

                            onConfigurationError: message => root.showPassiveNotification(TextFormatting.plainTextToHtml(message))
                        }

                        Controls.MenuItem {
                            text: KI18n.i18nc("@action", "Open Link")
                            icon.name: "document-open"
                            visible: terminalMenu.linkUrl.toString().length > 0

                            onTriggered: root.desktopIntegration.openUrl(terminalMenu.linkUrl)
                        }

                        Controls.MenuItem {
                            text: KI18n.i18nc("@action", "Copy Link")
                            icon.name: "edit-copy"
                            visible: terminalMenu.linkUrl.toString().length > 0

                            onTriggered: root.terminal.copyLink(terminalMenu.linkUrl)
                        }

                        Controls.MenuSeparator {
                            visible: terminalMenu.linkUrl.toString().length > 0
                        }

                        Controls.MenuItem {
                            action: copyAction
                        }

                        Controls.MenuItem {
                            action: copyCommandOutputAction
                        }

                        Controls.MenuItem {
                            action: pasteAction
                        }

                        Controls.MenuItem {
                            action: selectAllAction
                        }

                        Controls.MenuSeparator {}

                        Controls.MenuItem {
                            action: findAction
                        }

                        Instantiator {
                            model: webShortcuts.count > 0 ? 1 : 0

                            delegate: WebShortcutMenu {
                                providers: webShortcuts

                                onUrlRequested: url => root.desktopIntegration.openUrl(url)
                            }

                            onObjectAdded: (index, object) => terminalMenu.insertMenu(terminalMenu.count - 2, object)
                            onObjectRemoved: (index, object) => terminalMenu.removeMenu(object)
                        }

                        Controls.MenuSeparator {}

                        Controls.MenuItem {
                            action: openDirectoryAction
                        }
                    }
                }
            }
        }
    }

    Kirigami.PromptDialog {
        id: closeDialog

        objectName: "closeConfirmation"

        property double requestId: 0
        property int scope: SessionModel.Session
        property string sessionTitle
        property bool doNotAskAgain: false

        parent: Controls.Overlay.overlay
        anchors.centerIn: parent
        preferredWidth: Kirigami.Units.gridUnit * 28
        maximumWidth: Math.min(preferredWidth, absoluteMaximumWidth)
        closePolicy: Controls.Popup.CloseOnEscape
        dialogType: Kirigami.PromptDialog.Warning
        title: scope === SessionModel.Application ? KI18n.i18nc("@title:window", "Quit Kwi?") : scope === SessionModel.Window ? KI18n.i18nc("@title:window", "Close All Terminals?") : KI18n.i18nc("@title:window", "Close Terminal?")
        footer: PromptDialogFooter {
            id: closeDialogFooter

            actionRole: Controls.DialogButtonBox.DestructiveRole
            onRejected: closeDialog.reject()

            action: Kirigami.Action {
                text: closeDialog.scope === SessionModel.Application ? KI18n.i18nc("@action:button", "Quit Kwi") : closeDialog.scope === SessionModel.Window ? KI18n.i18nc("@action:button", "Close All Terminals") : KI18n.i18nc("@action:button", "Close Terminal")
                icon.name: "window-close"

                onTriggered: closeDialog.accept()
            }

            leadingComponent: Controls.CheckBox {
                objectName: "closeDoNotAskAgain"
                text: KI18n.i18nc("@option:check", "Do not ask again")
                enabled: !root.appSettings.isConfirmCloseRunningImmutable
                checked: closeDialog.doNotAskAgain

                onToggled: closeDialog.doNotAskAgain = checked
            }
        }

        Controls.Label {
            id: closeMessage

            objectName: "closeConfirmationMessage"
            Layout.fillWidth: true
            text: closeDialog.scope === SessionModel.Application ? KI18n.i18nc("@info", "Quitting will stop commands in all windows.") : closeDialog.scope === SessionModel.Window ? KI18n.i18nc("@info", "Closing this window will stop its running commands.") : KI18n.i18nc("@info", "Closing “%1” will stop its running command.", closeTitleMetrics.elidedText)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap

            TextMetrics {
                id: closeTitleMetrics

                font: closeMessage.font
                text: closeDialog.sessionTitle
                elide: Text.ElideMiddle
                elideWidth: closeMessage.width
            }
        }

        onAboutToShow: doNotAskAgain = false
        onOpened: closeDialogFooter.focusCancel()
        onAccepted: {
            const acceptedRequest = requestId;
            requestId = 0;
            root.sessionModel.confirmClose(acceptedRequest, doNotAskAgain);
            root.focusTerminal();
        }

        onRejected: {
            const rejectedRequest = requestId;
            requestId = 0;

            if (rejectedRequest) {
                root.sessionModel.cancelClose(rejectedRequest);
            }

            root.focusTerminal();
        }
    }

    Kirigami.PromptDialog {
        id: pasteDialog

        objectName: "pasteConfirmation"

        property string pasteContent
        property var targetSession: null
        property bool doNotAskAgain: false

        parent: Controls.Overlay.overlay
        anchors.centerIn: parent
        preferredWidth: Kirigami.Units.gridUnit * 34
        closePolicy: Controls.Popup.CloseOnEscape
        dialogType: Kirigami.PromptDialog.Warning
        title: KI18n.i18nc("@title:window", "Paste Multiple Lines?")
        footer: PromptDialogFooter {
            id: pasteDialogFooter

            onRejected: pasteDialog.reject()

            action: Kirigami.Action {
                text: KI18n.i18nc("@action:button", "Paste")
                icon.name: "edit-paste"

                onTriggered: pasteDialog.accept()
            }

            leadingComponent: Controls.CheckBox {
                objectName: "pasteDoNotAskAgain"
                text: KI18n.i18nc("@option:check", "Do not ask again")
                enabled: !root.appSettings.isConfirmMultilinePasteImmutable
                checked: pasteDialog.doNotAskAgain

                onToggled: pasteDialog.doNotAskAgain = checked
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                text: KI18n.i18nc("@info", "Pasting this text may run commands.")
                wrapMode: Text.WordWrap
            }

            Controls.ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 12, root.height / 3)
                contentWidth: availableWidth
                Controls.ScrollBar.horizontal.policy: Controls.ScrollBar.AlwaysOff

                Controls.TextArea {
                    objectName: "pastePreview"
                    text: pasteDialog.pasteContent.slice(0, 2000)
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    font.family: root.appSettings.fontFamily
                    wrapMode: TextEdit.Wrap
                    Accessible.name: KI18n.i18nc("@info:accessibility", "Paste preview")
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: pasteDialog.pasteContent.length > 2000
                text: KI18n.i18ncp("@info", "Showing the first character.", "Showing the first %1 characters.", 2000)
                wrapMode: Text.WordWrap
            }
        }

        onAboutToShow: doNotAskAgain = false
        onOpened: pasteDialogFooter.focusCancel()
        onAccepted: {
            if (targetSession && targetSession === root.terminal.session) {
                if (doNotAskAgain) {
                    root.appSettings.confirmMultilinePaste = false;
                    root.appSettings.save();
                }

                targetSession.pasteApprovedText(pasteContent);
            }

            pasteContent = "";
            targetSession = null;
            root.focusTerminal();
        }

        onRejected: {
            pasteContent = "";
            targetSession = null;
            root.focusTerminal();
        }
    }

    Controls.Menu {
        id: tabMenu

        objectName: "tabContextMenu"
        property TerminalSession targetSession: null

        function targetIndex(): int {
            return root.sessionModel.indexOf(targetSession);
        }

        onAboutToShow: {
            const index = targetIndex();
            contextMoveLeft.enabled = index > 0;
            contextMoveRight.enabled = index >= 0 && index < root.sessionModel.count - 1;
        }

        Controls.MenuItem {
            objectName: "tabContextRename"
            text: renameTabAction.text
            icon.name: renameTabAction.icon.name
            enabled: tabMenu.targetSession !== null

            onTriggered: root.showRename(tabMenu.targetSession)
        }

        Controls.MenuItem {
            id: contextMoveLeft

            objectName: "tabContextMoveLeft"
            text: moveTabLeftAction.text
            icon.name: moveTabLeftAction.icon.name

            onTriggered: root.sessionModel.moveSession(tabMenu.targetIndex(), tabMenu.targetIndex() - 1)
        }

        Controls.MenuItem {
            id: contextMoveRight

            objectName: "tabContextMoveRight"
            text: moveTabRightAction.text
            icon.name: moveTabRightAction.icon.name

            onTriggered: root.sessionModel.moveSession(tabMenu.targetIndex(), tabMenu.targetIndex() + 1)
        }

        Controls.MenuItem {
            objectName: "tabContextDetach"
            text: detachTabAction.text
            icon.name: detachTabAction.icon.name
            enabled: tabMenu.targetSession !== null && root.sessionModel.count > 1

            onTriggered: root.sessionModel.detachSession(tabMenu.targetIndex())
        }

        Controls.MenuSeparator {}

        Controls.MenuItem {
            objectName: "tabContextClose"
            text: closeTabAction.text
            icon.name: closeTabAction.icon.name
            enabled: tabMenu.targetSession !== null

            onTriggered: root.sessionModel.closeSession(tabMenu.targetIndex())
        }
    }

    Kirigami.PromptDialog {
        id: renameDialog

        objectName: "renameDialog"

        property TerminalSession targetSession: null

        parent: Controls.Overlay.overlay
        anchors.centerIn: parent
        preferredWidth: Kirigami.Units.gridUnit * 26
        closePolicy: Controls.Popup.CloseOnEscape
        title: KI18n.i18nc("@title:window", "Rename Tab")
        footer: PromptDialogFooter {
            onRejected: renameDialog.reject()

            action: Kirigami.Action {
                text: KI18n.i18nc("@action:button", "Rename")
                icon.name: "edit-rename"

                onTriggered: renameDialog.accept()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true

            Controls.TextField {
                id: renameField

                objectName: "renameField"
                Layout.fillWidth: true
                maximumLength: 128
                placeholderText: KI18n.i18nc("@info:placeholder", "Automatic title")
                Accessible.name: KI18n.i18nc("@label:textbox", "Tab title")

                onAccepted: renameDialog.accept()
            }

            Controls.Label {
                Layout.fillWidth: true
                text: KI18n.i18nc("@info", "Leave empty to use the automatic title.")
                wrapMode: Text.WordWrap
            }
        }

        onOpened: {
            renameField.forceActiveFocus();
            renameField.selectAll();
        }

        onAccepted: {
            if (root.sessionModel.indexOf(targetSession) >= 0)
                targetSession.customTitle = renameField.text;
        }

        onClosed: {
            targetSession = null;
            root.focusTerminal();
        }
    }

    KirigamiDialogs.SearchDialog {
        id: chooserDialog

        objectName: "chooserDialog"

        property bool choosingLinks: false
        property var entries: []

        parent: Controls.Overlay.overlay
        height: Math.min(Kirigami.Units.gridUnit * 20, parent.height - topMargin - bottomMargin)
        model: entries.filter(entry => (entry.label + (choosingLinks ? " " + entry.url : "")).toLocaleLowerCase().includes(text.toLocaleLowerCase()))
        searchFieldPlaceholderText: choosingLinks ? KI18n.i18nc("@info:placeholder", "Filter links…") : KI18n.i18nc("@info:placeholder", "Filter tabs…")
        emptyText: choosingLinks ? (entries.length === 0 ? KI18n.i18nc("@info", "No links in the visible terminal output.") : KI18n.i18nc("@info", "No matching links.")) : KI18n.i18nc("@info", "No matching tabs.")

        footer: Controls.DialogButtonBox {
            visible: chooserDialog.choosingLinks

            Controls.Button {
                objectName: "copyLinkButton"
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.ActionRole
                enabled: chooserDialog.currentItem !== null
                text: KI18n.i18nc("@action:button", "Copy Link")
                icon.name: "edit-copy"

                onClicked: {
                    root.terminal.copyLink((chooserDialog.currentItem as ChooserDelegate).modelData.url);
                    chooserDialog.close();
                }
            }
        }

        component ChooserDelegate: Controls.ItemDelegate {
            id: chooserDelegate

            required property int index
            required property var modelData

            width: ListView.view.width
            text: modelData.label
            Accessible.description: chooserDialog.choosingLinks ? (modelData.url?.toString() ?? "") : ""
            highlighted: ListView.isCurrentItem

            contentItem: KirigamiDelegates.TitleSubtitle {
                title: chooserDelegate.text
                subtitle: chooserDialog.choosingLinks ? (chooserDelegate.modelData.url?.toString() ?? "") : ""
                selected: chooserDelegate.highlighted || chooserDelegate.down
                font: chooserDelegate.font
                textFormat: Text.PlainText
            }

            onClicked: root.chooseEntry(modelData)
        }

        delegate: ChooserDelegate {}

        onAccepted: {
            if (currentItem)
                (currentItem as ChooserDelegate).clicked();
        }

        onClosed: root.focusTerminal()
    }

    Dialogs.FileDialog {
        id: saveDialog

        objectName: "saveOutputDialog"

        property var targetSession: null

        title: KI18n.i18nc("@title:window", "Save Terminal Output")
        fileMode: Dialogs.FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: [KI18n.i18nc("@item:inlistbox", "Text files (*.txt)"), KI18n.i18nc("@item:inlistbox", "All files (*)")]

        onAccepted: {
            root.desktopIntegration.saveOutput(selectedFile, targetSession);
            targetSession = null;
            root.focusTerminal();
        }

        onRejected: {
            targetSession = null;
            root.focusTerminal();
        }
    }

    component InformationWindow: QtObject {
        id: informationWindow

        required property var pageFactory
        required property string title
        property var initialProperties: ({})
        property var page: null
        readonly property var window: page?.Window.window ?? null
        readonly property bool visible: window?.visible ?? false

        function open(): void {
            if (window) {
                window.raise();
                window.requestActivate();

                return;
            }

            page = root.pageStack.pushDialogLayer(pageFactory(), initialProperties, {
                objectName: objectName + "Window",
                title: title,
                modality: Qt.WindowModal,
                width: Kirigami.Units.gridUnit * 30,
                height: Kirigami.Units.gridUnit * 30
            });
        }

        property Connections windowConnection: Connections {
            target: informationWindow.window

            function onVisibleChanged(): void {
                if (!informationWindow.visible) {
                    informationWindow.page = null;

                    if (root.terminalActionsEnabled) {
                        root.focusTerminal();
                    }
                }
            }
        }
    }

    InformationWindow {
        id: aboutKdeDialog

        objectName: "aboutKdeDialog"
        title: KI18n.i18nc("@title:window", "About KDE")
        pageFactory: () => Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutKDEPage")
        initialProperties: ({ objectName: "aboutKdePage" })
    }

    InformationWindow {
        id: aboutDialog

        objectName: "aboutDialog"
        title: KI18n.i18nc("@title:window", "About Kwi")
        pageFactory: () => Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutPage")
        initialProperties: ({ objectName: "aboutPage", getInvolvedUrl: "", donateUrl: "" })
    }
}
