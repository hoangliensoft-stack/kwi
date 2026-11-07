// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

Q_MOC_INCLUDE("TerminalSession.h")

class KNotification;
class QWindow;

namespace Kwi
{
class SessionModel;
class TerminalSession;

class DesktopIntegration final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Desktop services belong to their window or embedded host.")
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

public:
    // Call after constructing QGuiApplication and selecting the desktop icon theme.
    static void initializeApplicationIcon();

    enum class Mode {
        Standalone,
        Embedded
    };

    explicit DesktopIntegration(SessionModel *sessions, Mode mode = Mode::Standalone, QObject *parent = nullptr);

    void setWindow(QWindow *window);
    void setSessionFocused(bool focused);
    Q_INVOKABLE void toggleFullScreen();
    Q_INVOKABLE void openUrl(const QUrl &url);
    Q_INVOKABLE void saveOutput(const QUrl &url, Kwi::TerminalSession *session);
    Q_INVOKABLE void openCurrentDirectory(Kwi::TerminalSession *session);
    // Mounts the SSH server of \a url with \a password, then opens \a url.
    // Answers remotePasswordRequested().
    Q_INVOKABLE void openRemoteFileWithPassword(const QUrl &url, const QString &password);
    // Lists the SSH server directory \a url for path completion, mounting the
    // server with \a password first when it is not empty. Answers with
    // remoteDirectoryListed().
    Q_INVOKABLE void listRemoteDirectory(const QUrl &url, const QString &password = QString{});
    [[nodiscard]] QString errorString() const;
    Q_INVOKABLE void clearError();

Q_SIGNALS:
    void errorChanged();
    void sessionActivationRequested(Kwi::TerminalSession *session);
    // The SSH server of \a url needs a password before the file can be opened.
    // \a error describes why a previous attempt failed, if any.
    void remotePasswordRequested(const QUrl &url, const QString &server, const QString &error);
    // Entries of a listRemoteDirectory() request. Directory names end in "/".
    // When \a passwordRequired is true, request the listing again with a password.
    void remoteDirectoryListed(const QUrl &url, const QStringList &entries, const QString &error, bool passwordRequired);

private:
    void notifyBell(TerminalSession *session);
    void notify(TerminalSession *session, const QString &event, const QString &title, const QString &text);
    void dismissNotification(TerminalSession *session);
    void dismissCurrentNotification();
    void activateSession(TerminalSession *session, const QString &activationToken);
    void watchSession(TerminalSession *session);
    void setError(const QString &error);
    void launchHostCommand(const QString &program, const QStringList &arguments);
    void openRemoteFile(const QUrl &url);
    void openMountedRemoteFile(const QUrl &url, const QString &method);
    void runHostScript(const QString &script,
                       const QStringList &arguments,
                       const QByteArray &input,
                       std::function<void(int exitCode, const QString &output)> done);

    SessionModel *m_sessions;
    QPointer<QWindow> m_window{};
    QHash<TerminalSession *, QPointer<KNotification>> m_notifications{};
    QElapsedTimer m_lastNotification{};
    QString m_error{};
    Mode m_mode;
    bool m_sessionFocused;
};
}
