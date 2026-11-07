// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import org.kde.ki18n
import org.kde.kwi
import org.kde.kwi.terminal as TerminalComponents

TerminalComponents.Terminal {
    id: root

    property TerminalSession applicationSession: null
    readonly property AppSettings settings: applicationSession ? applicationSession.settings : null
    readonly property alias terminal: root.view

    session: applicationSession
    view.Accessible.name: applicationSession ? KI18n.i18nc("@info:accessibility", "Terminal: %1", applicationSession.title) : KI18n.i18nc("@info:accessibility", "Terminal")
    view.fontFamily: settings && !settings.useSystemFont ? settings.fontFamily : ""
    view.fontPointSize: settings && !settings.useSystemFont ? settings.fontPointSize : 0
    view.colorScheme: settings ? settings.colorScheme : "system"
    view.backgroundOpacity: settings ? settings.backgroundOpacity : 1
    view.cursorStyle: settings ? settings.cursorStyle : "block"
    view.cursorBlinking: settings ? settings.cursorBlinking : false
    view.syntaxHighlighting: settings ? settings.syntaxHighlighting : false
    view.contentPadding: settings ? settings.terminalPadding : 1
    view.confirmMultilinePaste: settings ? settings.confirmMultilinePaste : true
    view.copyOnSelect: settings ? settings.copyOnSelect : false
    view.requireCtrlForLinks: settings ? settings.requireCtrlForLinks : true
}
