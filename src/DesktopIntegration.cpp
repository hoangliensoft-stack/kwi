// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "DesktopIntegration.h"
#include "OutputSaveJob.h"
#include "SessionModel.h"
#include "TerminalSession.h"

#include <KIO/CommandLauncherJob>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KNotification>
#include <KSandbox>
#include <KWaylandExtras>
#include <KWindowSystem>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <memory>
#include <utility>

namespace Kwi
{
void DesktopIntegration::initializeApplicationIcon()
{
    const QString resourcePath{QStringLiteral(":/icons")};

    // Append the bundled icons so the desktop theme takes precedence.
    if (!QIcon::themeSearchPaths().contains(resourcePath)) {
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << resourcePath);
    }

    if (!QIcon::fallbackSearchPaths().contains(resourcePath)) {
        QIcon::setFallbackSearchPaths(QIcon::fallbackSearchPaths() << resourcePath);
    }

    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.kwi")));
}

namespace
{
constexpr qsizetype maximumUrlLength{8192};
constexpr qsizetype maximumNotificationLength{8192};
constexpr qint64 notificationIntervalMs{2000};
constexpr int hostScriptTimeoutMs{60000};
constexpr qsizetype maximumHostScriptOutput{1024 * 1024};
// Exit status of the remote mount scripts when the server asks for a password.
constexpr int passwordRequiredExitCode{10};

QString remoteServerName(const QUrl &url)
{
    return url.userName().isEmpty() ? url.host() : url.userName() + QLatin1Char('@') + url.host();
}

QUrl remoteRoot(const QUrl &url)
{
    QUrl root{url};
    root.setPath(QStringLiteral("/"));
    root.setQuery(QString{});
    root.setFragment(QString{});

    return root;
}
}

DesktopIntegration::DesktopIntegration(SessionModel *sessions, Mode mode, QObject *parent)
    : QObject{parent}
    , m_sessions{sessions}
    , m_mode{mode}
    , m_sessionFocused{mode == Mode::Standalone}
{
    connect(sessions, &SessionModel::sessionAdded, this, &DesktopIntegration::watchSession);

    for (int i{0}; i < sessions->count(); ++i) {
        watchSession(sessions->data(sessions->index(i), SessionModel::SessionRole).value<TerminalSession *>());
    }

    connect(sessions, &SessionModel::sessionRemoved, this, &DesktopIntegration::dismissNotification);
    connect(sessions, &SessionModel::sessionRemoved, this, [this](TerminalSession *session) {
        disconnect(session, nullptr, this, nullptr);
    });

    connect(sessions, &SessionModel::currentIndexChanged, this, &DesktopIntegration::dismissCurrentNotification);
}

void DesktopIntegration::watchSession(TerminalSession *session)
{
    connect(session, &TerminalSession::bell, this, [this, session] {
        notifyBell(session);
    });

    connect(session, &TerminalSession::notificationRequested, this, [this, session](const QString &title, const QString &body) {
        const QString text{title.isEmpty() ? body : title + QLatin1Char('\n') + body};
        notify(session, QStringLiteral("notification"), i18n("Terminal Notification"), text);
    });

    connect(session, &TerminalSession::activityDetected, this, [this, session] {
        notify(session, QStringLiteral("activity"), i18n("Terminal Activity"), i18n("New output in %1", session->title()));
    });

    connect(session, &TerminalSession::silenceDetected, this, [this, session] {
        notify(session,
               QStringLiteral("silence"),
               i18n("Terminal Silence"),
               i18np("No output in %2 for %1 second", "No output in %2 for %1 seconds", session->silenceSeconds(), session->title()));
    });
}

void DesktopIntegration::setWindow(QWindow *window)
{
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
    }

    m_window = window;

    if (window) {
        connect(window, &QWindow::activeChanged, this, &DesktopIntegration::dismissCurrentNotification);
    }
}

void DesktopIntegration::setSessionFocused(bool focused)
{
    if (m_sessionFocused != focused) {
        m_sessionFocused = focused;
        dismissCurrentNotification();
    }
}

void DesktopIntegration::toggleFullScreen()
{
    if (!m_window) {
        return;
    }

    // Preserve Maximized when entering full screen so Wayland restores it on exit.
    auto states = m_window->windowStates();
    states.setFlag(Qt::WindowFullScreen, !states.testFlag(Qt::WindowFullScreen));
    m_window->setWindowStates(states);
}

QString DesktopIntegration::errorString() const
{
    return m_error;
}

void DesktopIntegration::clearError()
{
    if (!m_error.isEmpty()) {
        m_error.clear();

        Q_EMIT errorChanged();
    }
}

void DesktopIntegration::setError(const QString &error)
{
    if (m_error == error) {
        return;
    }

    m_error = error;

    Q_EMIT errorChanged();
}

void DesktopIntegration::saveOutput(const QUrl &url, TerminalSession *session)
{
    if (!session || m_sessions->indexOf(session) < 0) {
        setError(i18n("This terminal is no longer available."));

        return;
    }

    if (!url.isLocalFile() || url.toLocalFile().isEmpty()) {
        setError(i18n("Choose a local file to save terminal output."));

        return;
    }

    auto *job = new OutputSaveJob{session->terminal(), url.toLocalFile(), this};
    clearError();

    connect(job, &KJob::result, this, [this](KJob *finished) {
        if (finished->error()) {
            setError(i18n("Could not save terminal output: %1", finished->errorString()));
        }
    });

    job->start();
}

void DesktopIntegration::openCurrentDirectory(TerminalSession *session)
{
    if (!session || m_sessions->indexOf(session) < 0) {
        setError(i18n("This terminal is no longer available."));

        return;
    }

    const QString directory{session->workingDirectory()};

    if (directory.isEmpty() || (!KSandbox::isFlatpak() && !QDir(directory).exists())) {
        setError(i18n("The terminal's current folder is no longer available."));

        return;
    }

    // Keep this a directory launch even if the path is replaced before opening.
    const QUrl url{QUrl::fromLocalFile(directory.endsWith(QLatin1Char('/')) ? directory : directory + QLatin1Char('/'))};
    openUrl(url);
}

void DesktopIntegration::launchHostCommand(const QString &program, const QStringList &arguments)
{
    const auto launch = [this, program, arguments](const QString &token) {
        QProcess process{};
        process.setProgram(program);
        process.setArguments(arguments);

        if (!token.isEmpty()) {
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), token);
            process.setProcessEnvironment(environment);
        }

        const auto context = KSandbox::makeHostContext(process);
        auto *job = new KIO::CommandLauncherJob{context.program, context.arguments, this};
        job->setStartupId(token.toUtf8());
        job->setUiDelegate(nullptr);
        clearError();

        connect(job, &KJob::result, this, [this](KJob *finished) {
            if (finished->error()) {
                setError(i18n("Could not launch the desktop application: %1", finished->errorString()));
            }
        });

        job->start();
    };

    if (m_window && KWindowSystem::isPlatformWayland()) {
        KWaylandExtras::xdgActivationToken(m_window, {}).then(this, launch);
    } else {
        launch({});
    }
}

void DesktopIntegration::openUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().isEmpty() || url.toEncoded().size() > maximumUrlLength) {
        return;
    }

    if (url.scheme() == u"sftp") {
        openRemoteFile(url);

        return;
    }

    if (KSandbox::isFlatpak() && url.isLocalFile()) {
        // Host paths may be inaccessible to the file portal inside the sandbox.
        // kde-open and gio open use the host's handler without executing the
        // file. Desktops without either, such as GNOME without KDE tools, fall
        // back to xdg-open.
        static const QString script{QStringLiteral(
            "command -v kde-open >/dev/null 2>&1 && exec kde-open \"$1\"\n"
            "command -v gio >/dev/null 2>&1 && exec gio open \"$1\"\n"
            "exec xdg-open \"$1\"\n")};

        launchHostCommand(QStringLiteral("sh"), {QStringLiteral("-c"), script, QStringLiteral("kwi-open"), url.toString(QUrl::FullyEncoded)});

        return;
    }

    auto *job = new KIO::OpenUrlJob{url, this};
    job->setRunExecutables(false);
    job->setUiDelegate(nullptr);
    clearError();

    connect(job, &KJob::result, this, [this](KJob *finished) {
        if (finished->error()) {
            setError(i18n("Could not open the link: %1", finished->errorString()));
        }
    });

    job->start();
}

void DesktopIntegration::openRemoteFile(const QUrl &url)
{
    // Open SSH server files with the host's own handler, so the desktop's
    // editor saves them in place through KIO or GVfs. Neither a Flatpak sandbox
    // nor a native GNOME session can rely on Kwi's KIO having an sftp worker.
    // KIO asks for passwords itself. GVfs only opens a mounted location, so
    // mount the server first. gio mount reads a password from standard input:
    // when the SSH key or agent does not authenticate, ask for one in Kwi's
    // window and mount again with openRemoteFileWithPassword().
    static const QString script{QStringLiteral(
        "root=$1\n"
        "case \"${XDG_CURRENT_DESKTOP:-}\" in *KDE*) command -v kde-open >/dev/null 2>&1 && { echo kde; exit 0; } ;; esac\n"
        "command -v gio >/dev/null 2>&1 || { echo xdg; exit 0; }\n"
        "gio info \"$root\" >/dev/null 2>&1 && { echo gio; exit 0; }\n"
        "output=$(LC_ALL=C gio mount \"$root\" </dev/null 2>&1) && { echo gio; exit 0; }\n"
        "case \"$output\" in *'Authentication Required'*) exit 10 ;; esac\n"
        "printf '%s\\n' \"$output\"\n"
        "exit 1\n")};

    clearError();

    runHostScript(script, {remoteRoot(url).toString(QUrl::FullyEncoded)}, {}, [this, url](int exitCode, const QString &output) {
        if (exitCode == 0) {
            openMountedRemoteFile(url, output.trimmed());
        } else if (exitCode == passwordRequiredExitCode && m_mode == Mode::Standalone) {
            Q_EMIT remotePasswordRequested(url, remoteServerName(url), QString{});
        } else if (exitCode == passwordRequiredExitCode) {
            setError(i18n("%1 asks for a password. Set up an SSH key or agent to open its files.", remoteServerName(url)));
        } else {
            setError(i18n("Could not connect to %1: %2", remoteServerName(url), output.trimmed()));
        }
    });
}

void DesktopIntegration::openRemoteFileWithPassword(const QUrl &url, const QString &password)
{
    if (!url.isValid() || url.scheme() != u"sftp" || url.toEncoded().size() > maximumUrlLength) {
        return;
    }

    // The password reaches gio on standard input, never on a command line.
    // A rejected password makes gio ask again and fail at the end of input.
    static const QString script{QStringLiteral(
        "output=$(LC_ALL=C gio mount \"$1\" 2>&1) && exit 0\n"
        "case \"$output\" in *'Authentication Required'*) exit 10 ;; esac\n"
        "printf '%s\\n' \"$output\"\n"
        "exit 1\n")};

    clearError();

    runHostScript(script,
                  {remoteRoot(url).toString(QUrl::FullyEncoded)},
                  password.toUtf8() + '\n',
                  [this, url](int exitCode, const QString &output) {
                      if (exitCode == 0) {
                          openMountedRemoteFile(url, QStringLiteral("gio"));
                      } else if (exitCode == passwordRequiredExitCode) {
                          Q_EMIT remotePasswordRequested(url, remoteServerName(url), i18n("The password was not accepted."));
                      } else {
                          setError(i18n("Could not connect to %1: %2", remoteServerName(url), output.trimmed()));
                      }
                  });
}

void DesktopIntegration::listRemoteDirectory(const QUrl &url, const QString &password)
{
    if (!url.isValid() || url.scheme() != u"sftp" || url.toEncoded().size() > maximumUrlLength) {
        return;
    }

    // Mount like openRemoteFile(), reading a password from standard input only
    // when one was given, then list the directory through GVfs.
    static const QString script{QStringLiteral(
        "root=$1 directory=$2\n"
        "command -v gio >/dev/null 2>&1 || { echo 'gio is not installed.'; exit 1; }\n"
        "if ! gio info \"$root\" >/dev/null 2>&1; then\n"
        "    if [ \"$3\" = password ]; then\n"
        "        output=$(LC_ALL=C gio mount \"$root\" 2>&1)\n"
        "    else\n"
        "        output=$(LC_ALL=C gio mount \"$root\" </dev/null 2>&1)\n"
        "    fi || {\n"
        "        case \"$output\" in *'Authentication Required'*) exit 10 ;; esac\n"
        "        printf '%s\\n' \"$output\"\n"
        "        exit 1\n"
        "    }\n"
        "fi\n"
        "exec env LC_ALL=C gio list -h -l -a standard::type \"$directory\"\n")};

    const bool withPassword{!password.isEmpty()};

    runHostScript(script,
                  {remoteRoot(url).toString(QUrl::FullyEncoded), url.toString(QUrl::FullyEncoded), withPassword ? QStringLiteral("password") : QString{}},
                  withPassword ? password.toUtf8() + '\n' : QByteArray{},
                  [this, url, withPassword](int exitCode, const QString &output) {
                      if (exitCode == passwordRequiredExitCode) {
                          Q_EMIT remoteDirectoryListed(url, {}, withPassword ? i18n("The password was not accepted.") : QString{}, true);

                          return;
                      }

                      if (exitCode != 0) {
                          QString error{output.trimmed()};
                          error.remove(QRegularExpression(QStringLiteral(R"(^gio: \S+: )")));
                          Q_EMIT remoteDirectoryListed(url, {}, error, false);

                          return;
                      }

                      QStringList entries;
                      const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

                      for (const QString &line : lines) {
                          const auto fields = line.split(QLatin1Char('\t'));

                          if (fields.size() < 3 || fields.first().isEmpty() || fields.first() == u"." || fields.first() == u"..") {
                              continue;
                          }

                          entries.append(fields.last() == u"(directory)" ? fields.first() + QLatin1Char('/') : fields.first());
                      }

                      entries.sort();
                      Q_EMIT remoteDirectoryListed(url, entries, QString{}, false);
                  });
}

void DesktopIntegration::openMountedRemoteFile(const QUrl &url, const QString &method)
{
    // Report failures as a notification, since the launched script has no other UI.
    static const QString script{QStringLiteral(
        "url=$1\n"
        "case \"$2\" in\n"
        "kde) exec kde-open \"$url\" ;;\n"
        "gio)\n"
        "    error=$(gio open \"$url\" 2>&1) && exit 0\n"
        "    command -v notify-send >/dev/null 2>&1 && notify-send -a Kwi -i dialog-error \"$3\" \"$error\"\n"
        "    exit 1 ;;\n"
        "esac\n"
        "exec xdg-open \"$url\"\n")};

    launchHostCommand(QStringLiteral("sh"),
                      {QStringLiteral("-c"),
                       script,
                       QStringLiteral("kwi-open-remote"),
                       url.toString(QUrl::FullyEncoded),
                       method,
                       i18n("Could not open %1", url.fileName().isEmpty() ? url.path() : url.fileName())});
}

void DesktopIntegration::runHostScript(const QString &script,
                                       const QStringList &arguments,
                                       const QByteArray &input,
                                       std::function<void(int exitCode, const QString &output)> done)
{
    QProcess setup{};
    setup.setProgram(QStringLiteral("sh"));
    setup.setArguments(QStringList{QStringLiteral("-c"), script, QStringLiteral("kwi-remote")} + arguments);
    const auto context = KSandbox::makeHostContext(setup);

    auto *process = new QProcess(this);
    auto *timeout = new QTimer(process);
    auto finish = std::make_shared<std::function<void(int, const QString &)>>(std::move(done));

    process->setProgram(context.program);
    process->setArguments(context.arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    timeout->setSingleShot(true);
    timeout->setInterval(hostScriptTimeoutMs);

    connect(timeout, &QTimer::timeout, process, [process] {
        process->kill();
    });

    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && *finish) {
            std::exchange(*finish, {})(-1, process->errorString());
            process->deleteLater();
        }
    });

    connect(process, &QProcess::finished, this, [process, finish](int exitCode, QProcess::ExitStatus status) {
        const QString output{QString::fromLocal8Bit(process->readAll().left(maximumHostScriptOutput))};
        process->deleteLater();

        if (*finish) {
            std::exchange(*finish, {})(status == QProcess::NormalExit ? exitCode : -1, output);
        }
    });

    process->start();
    timeout->start();

    if (!input.isEmpty()) {
        process->write(input);
    }

    process->closeWriteChannel();
}

void DesktopIntegration::notifyBell(TerminalSession *session)
{
    notify(session, QStringLiteral("bell"), i18n("Terminal Bell"), session->title());
}

void DesktopIntegration::notify(TerminalSession *session, const QString &event, const QString &title, const QString &text)
{
    if (!m_window || m_sessions->indexOf(session) < 0 || (m_sessionFocused && m_window->isActive() && m_sessions->currentSession() == session)) {
        return;
    }

    session->setAttention(true);
    const bool monitoring{event == QStringLiteral("activity") || event == QStringLiteral("silence")};

    if (auto previous = m_notifications.value(session)) {
        if (!monitoring || previous->eventId() == event) {
            return;
        }

        dismissNotification(session);
    }

    if (!monitoring && m_lastNotification.isValid() && m_lastNotification.elapsed() < notificationIntervalMs) {
        return;
    }

    auto *notification = new KNotification{event, KNotification::CloseOnTimeout, this};
    notification->setComponentName(QStringLiteral("kwi"));
    notification->setIconName(QStringLiteral("org.kde.kwi"));
    notification->setTitle(title);
    notification->setText(text.left(maximumNotificationLength).toHtmlEscaped());
    notification->setUrgency(KNotification::NormalUrgency);
    const QPointer<TerminalSession> owner{session};

    connect(notification->addDefaultAction(i18n("Show Terminal")), &KNotificationAction::activated, this, [this, owner, notification] {
        if (owner) {
            activateSession(owner, notification->xdgActivationToken());
        }
    });

    m_notifications.insert(session, notification);
    m_lastNotification.start();
    notification->sendEvent();
}

void DesktopIntegration::dismissNotification(TerminalSession *session)
{
    if (const auto notification = m_notifications.take(session)) {
        notification->close();
    }
}

void DesktopIntegration::dismissCurrentNotification()
{
    if (m_sessionFocused && m_window && m_window->isActive()) {
        if (auto *session = m_sessions->currentSession()) {
            session->setAttention(false);
        }

        dismissNotification(m_sessions->currentSession());
    }
}

void DesktopIntegration::activateSession(TerminalSession *session, const QString &activationToken)
{
    const int index{m_sessions->indexOf(session)};

    if (!m_window || index < 0) {
        return;
    }

    m_sessions->setCurrentIndex(index);
    m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
    m_window->setVisible(true);

    if (!activationToken.isEmpty()) {
        KWindowSystem::setCurrentXdgActivationToken(activationToken);
    }

    KWindowSystem::activateWindow(m_window);

    Q_EMIT sessionActivationRequested(session);
}
}
