// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Session.h"
#include "ProcessLaunch.h"
#include "process/SshMetadataResolver_p.h"
#include <KLocalizedString>
#include <KUser>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaType>
#include <QMimeData>
#include <QSysInfo>
#include <QUrl>
#include <cmath>
#include <utility>

namespace Kwi
{
namespace
{
QString terminalErrorMessage(GhosttyQt::Terminal::Error error)
{
    switch (error) {
    case GhosttyQt::Terminal::Error::None:
        return {};
    case GhosttyQt::Terminal::Error::AllocationFailed:
        return i18nd("kwi-terminal", "Could not allocate terminal state.");
    case GhosttyQt::Terminal::Error::RenderStateFailed:
        return i18nd("kwi-terminal", "Could not capture terminal render state.");
    }

    Q_UNREACHABLE();
}
}

Session::Session(QObject *parent)
    : QObject{parent}
    , m_sshMetadata{std::make_unique<SshMetadataResolver>()}
    , m_launchedProgramName{i18nd("kwi-terminal", "Terminal")}
    , m_localUserName{KUser().loginName()}
    , m_localHostName{QSysInfo::machineHostName()}
    , m_homeDirectory{QDir::homePath()}
{
    connect(m_sshMetadata.get(), &SshMetadataResolver::changed, this, &Session::sshInfoChanged);
    connect(m_sshMetadata.get(), &SshMetadataResolver::changed, this, &Session::remoteWorkingDirectoryChanged);
    connect(this, &Session::applicationTitleChanged, this, &Session::remoteWorkingDirectoryChanged);
    m_terminal = std::make_unique<GhosttyQt::Terminal>(m_columns, m_rows, m_scrollbackLines);

    if (!m_terminal->isValid()) {
        m_error = terminalErrorMessage(m_terminal->error());
        m_terminal.reset();
        m_state = State::Failed;
    } else {
        m_terminal->setTerminfoName("xterm-256color");
        m_terminal->setApplicationIdentity(QByteArray("Kwi (libghostty " + GhosttyQt::Terminal::version().toUtf8() + ")"));
        m_terminal->setClipboardWriteHandler([](std::unique_ptr<QMimeData> contents, QClipboard::Mode mode) {
            auto *clipboard = QGuiApplication::clipboard();

            if (mode == QClipboard::Selection && !clipboard->supportsSelection()) {
                return false;
            }

            if (contents) {
                clipboard->setMimeData(contents.release(), mode);
            } else {
                clipboard->clear(mode);
            }

            return true;
        });

        // Read-only mode still allows terminal protocol replies.
        connect(m_terminal.get(), &GhosttyQt::Terminal::writeRequested, &m_process, &PtyProcess::write);
        connect(m_terminal.get(), &GhosttyQt::Terminal::titleChanged, this, [this](const QString &value) {
            QString title{value.left(256)};

            for (QChar &character : title) {
                if (character.isNonCharacter() || character.category() == QChar::Other_Control) {
                    character = QLatin1Char(' ');
                }
            }

            if (!title.isEmpty() && title.back().isHighSurrogate()) {
                title.chop(1);
            }

            if (!m_hasApplicationTitle || m_applicationTitle != title) {
                m_hasApplicationTitle = true;
                m_applicationTitle = title;

                Q_EMIT applicationTitleChanged();
            }
        });

        connect(m_terminal.get(), &GhosttyQt::Terminal::workingDirectoryChanged, this, &Session::refreshSessionMetadata);
        connect(m_terminal.get(), &GhosttyQt::Terminal::workingDirectoryChanged, this, &Session::remoteWorkingDirectoryChanged);
        connect(m_terminal.get(), &GhosttyQt::Terminal::bell, this, &Session::bell);
        connect(m_terminal.get(), &GhosttyQt::Terminal::frameChanged, &m_events, &SessionEvents::frameChanged);
        connect(m_terminal.get(), &GhosttyQt::Terminal::errorOccurred, this, [this](GhosttyQt::Terminal::Error error) {
            reportError(terminalErrorMessage(error));
        });
    }

    connect(&m_process, &PtyProcess::processInfoChanged, this, &Session::refreshSessionMetadata);
    connect(&m_process, &PtyProcess::dataReceived, this, &Session::feed);
    m_silenceTimer.setSingleShot(true);
    m_silenceTimer.setTimerType(Qt::PreciseTimer);

    connect(&m_silenceTimer, &QTimer::timeout, this, [this] {
        if (m_monitorSilence && !isFinished() && !m_closeRequested) {
            Q_EMIT silenceDetected();
        }
    });

    connect(&m_process, &PtyProcess::started, this, [this] {
        m_started = true;
        const QString name{QFileInfo(m_process.program()).fileName()};

        if (!name.isEmpty() && m_launchedProgramName != name) {
            m_launchedProgramName = name;

            Q_EMIT launchedProgramNameChanged();
        }

        if (!m_closeRequested) {
            setState(State::Running);
        }

        if (m_monitorSilence) {
            m_silenceTimer.start(silenceSeconds() * 1000);
        }

        refreshSessionMetadata();

        Q_EMIT started();
    });

    connect(&m_process, &PtyProcess::errorOccurred, this, &Session::reportError);
    connect(&m_process, &PtyProcess::exited, this, &Session::finish);
}

Session::~Session() = default;

QString Session::executable() const
{
    return m_executable;
}

void Session::setExecutable(const QString &newExecutable)
{
    if (isActive() || m_closeRequested || m_executable == newExecutable) {
        return;
    }

    m_executable = newExecutable;

    Q_EMIT launchChanged();
}

QStringList Session::arguments() const
{
    return m_arguments;
}

void Session::setArguments(const QStringList &newArguments)
{
    if (isActive() || m_closeRequested || m_arguments == newArguments) {
        return;
    }

    m_arguments = newArguments;

    Q_EMIT launchChanged();
}

QString Session::initialDirectory() const
{
    return m_initialDirectory;
}

void Session::setInitialDirectory(const QString &directory)
{
    if (isActive() || m_closeRequested || m_initialDirectory == directory) {
        return;
    }

    m_initialDirectory = directory;

    Q_EMIT launchChanged();
}

QVariantMap Session::environmentChanges() const
{
    return m_environmentChanges;
}

void Session::setEnvironmentChanges(const QVariantMap &changes)
{
    if (isActive() || m_closeRequested || m_environmentChanges == changes) {
        return;
    }

    m_environmentChanges = changes;

    Q_EMIT launchChanged();
}

bool Session::start()
{
    if (isActive() || m_closeRequested || !m_terminal) {
        return false;
    }

    TerminalLaunch launch;
    launch.program = m_executable;
    launch.arguments = m_arguments;
    launch.directory = m_initialDirectory;

    for (auto it = m_environmentChanges.cbegin(); it != m_environmentChanges.cend(); ++it) {
        const QVariant &value = it.value();

        if (!value.isNull() && value.metaType().id() != QMetaType::QString) {
            reportError(i18nd("kwi-terminal", "The terminal environment contains an invalid assignment."));

            return false;
        }

        launch.environmentChanges.insert(it.key(), value.isNull() ? std::nullopt : std::optional<QString>{value.toString()});
    }

    return start(launch);
}

bool Session::start(const TerminalLaunch &launch, int masterFd)
{
    if (!m_terminal || m_closeRequested || isActive()) {
        return false;
    }

    const QString error{launch.validationError()};

    if (!error.isEmpty()) {
        reportError(error);

        return false;
    }

    QVariantMap changes;

    for (auto it = launch.environmentChanges.cbegin(); it != launch.environmentChanges.cend(); ++it) {
        changes.insert(it.key(), it.value() ? QVariant{*it.value()} : QVariant::fromValue(nullptr));
    }

    const bool recipeChanged{m_executable != launch.program || m_arguments != launch.arguments || m_initialDirectory != launch.directory
                             || m_environmentChanges != changes};
    m_executable = launch.program;
    m_arguments = launch.arguments;
    m_initialDirectory = launch.directory;
    m_environmentChanges = std::move(changes);

    m_terminal->clearProcessMetadata();
    m_started = false;
    const bool exitCodeChanged{std::exchange(m_exitCode, 0) != 0};
    const QString name{launch.program.isEmpty() ? i18nd("kwi-terminal", "Terminal") : QFileInfo(launch.program).fileName()};
    const bool nameChanged{std::exchange(m_launchedProgramName, name) != name};
    const QString directory{launch.directory.isEmpty() ? QDir::homePath() : launch.directory};
    const bool directoryChanged{std::exchange(m_currentDirectory, directory) != directory};
    const bool hadTitle{std::exchange(m_hasApplicationTitle, false)};
    m_applicationTitle.clear();
    const bool hadError{!m_error.isEmpty()};
    m_error.clear();

    setState(State::Starting);

    if (recipeChanged) {
        Q_EMIT launchChanged();
    }

    if (exitCodeChanged) {
        Q_EMIT exitStatusChanged();
    }

    if (hadError) {
        Q_EMIT errorChanged();
    }

    if (nameChanged) {
        Q_EMIT launchedProgramNameChanged();
    }

    if (directoryChanged) {
        Q_EMIT workingDirectoryChanged(m_currentDirectory);
    }

    if (hadTitle) {
        Q_EMIT applicationTitleChanged();
    }

    if (m_closeRequested) {
        return false;
    }

    m_terminal->setTerminfoName(launch.environment().value(QStringLiteral("TERM")).toUtf8());
    TerminalLaunch resolved{launch};
    resolved.directory = directory;
    const bool accepted{m_process.start(resolved, m_columns, m_rows, masterFd)};

    if (!accepted) {
        finish(127);

        return false;
    }

    m_process.resize(m_columns, m_rows, m_cellPixels);

    return true;
}

bool Session::attachTeletype(int masterFd)
{
    if (!m_terminal || m_closeRequested || isActive()) {
        return false;
    }

    m_started = false;
    m_exitCode = 0;
    m_error.clear();

    setState(State::Starting);

    if (m_closeRequested) {
        return false;
    }

    const bool attached{m_process.attach(masterFd, m_columns, m_rows)};

    if (attached) {
        m_process.resize(m_columns, m_rows, m_cellPixels);
    } else {
        finish(127);
    }

    return attached;
}

Session::State Session::state() const
{
    return m_state;
}

void Session::setState(State newState)
{
    if (m_state == newState) {
        return;
    }

    const bool wasActive{isActive()};
    const bool wasFinished{isFinished()};
    m_state = newState;

    if (newState != State::Running) {
        m_sshMetadata->setProcess({});
    }

    Q_EMIT stateChanged();

    if (wasActive != isActive()) {
        Q_EMIT activeChanged();
    }

    if (wasFinished != isFinished()) {
        Q_EMIT exitStatusChanged();
    }
}

void Session::finish(int code)
{
    if (isFinished()) {
        return;
    }

    const State result{!m_started && !m_error.isEmpty() ? State::Failed : State::Exited};
    m_exitCode = code;
    m_progress = {};

    setMonitorActivity(false);
    setMonitorSilence(false);

    refreshSessionMetadata();

    Q_EMIT progressChanged();

    setState(result);

    Q_EMIT exited(code);
}

bool Session::isActive() const
{
    return m_state == State::Starting || m_state == State::Running || m_state == State::Closing;
}

bool Session::isFinished() const
{
    return m_state == State::Exited || m_state == State::Failed;
}

int Session::exitCode() const
{
    return m_exitCode;
}

void Session::close()
{
    if (std::exchange(m_closeRequested, true)) {
        return;
    }

    setMonitorActivity(false);
    setMonitorSilence(false);

    if (m_state == State::Starting && !m_started && !m_process.isRunning()) {
        // A host can cancel the launch from the Starting state notification.
        finish(0);
    } else if (isActive()) {
        setState(State::Closing);

        m_process.close();
    } else if (!isFinished()) {
        finish(0);
    }
}

SessionEvents *Session::events()
{
    return &m_events;
}

void Session::refreshProcessInfo()
{
    m_process.refreshProcessInfo([this] {
        Q_EMIT processInfoRefreshed();
    });
}

void Session::refreshProcessInfo(std::function<void()> completed)
{
    m_process.refreshProcessInfo(std::move(completed));
}

void Session::refreshSessionMetadata()
{
    if (!isFinished() && !m_closeRequested && (m_started || !isActive())) {
        const QString directory{observedDirectory()};

        if (directory != m_currentDirectory) {
            m_currentDirectory = directory;

            Q_EMIT workingDirectoryChanged(directory);
        }
    }

    const qint64 foregroundId{foregroundCommandId()};
    const QString foregroundName{foregroundId > 0 ? foregroundCommandName() : QString()};
    const bool foregroundChanged{foregroundId != m_notifiedForegroundId || foregroundName != m_notifiedForegroundName};
    m_notifiedForegroundId = foregroundId;
    m_notifiedForegroundName = foregroundName;
    m_sshMetadata->setProcess(m_state == State::Running ? m_process.sshProcessInfo() : SshProcessInfo{});

    Q_EMIT processInfoChanged();

    if (foregroundChanged) {
        Q_EMIT foregroundCommandChanged();
    }
}

QString Session::applicationTitle() const
{
    return m_applicationTitle;
}

bool Session::hasApplicationTitle() const
{
    return m_hasApplicationTitle;
}

QString Session::launchedProgramName() const
{
    return m_launchedProgramName;
}

QString Session::workingDirectory() const
{
    return m_currentDirectory;
}

QString Session::localUserName() const
{
    return m_localUserName;
}

QString Session::localHostName() const
{
    return m_localHostName;
}

QString Session::homeDirectory() const
{
    return m_homeDirectory;
}

bool Session::hasSshInfo() const
{
    return m_sshMetadata->available();
}

QString Session::sshUserName() const
{
    return m_sshMetadata->info().userName;
}

QString Session::sshHostName() const
{
    return m_sshMetadata->info().hostName;
}

int Session::sshPort() const
{
    return m_sshMetadata->info().port;
}

QString Session::sshRemoteCommand() const
{
    return m_sshMetadata->info().remoteCommand;
}

QString Session::remoteWorkingDirectory() const
{
    if (!m_terminal || !hasSshInfo()) {
        return {};
    }

    const QUrl url{m_terminal->workingDirectoryUrl()};
    const QString host{url.host()};

    // Only a directory reported by another host belongs to the SSH server.
    if (url.isLocalFile() && !host.isEmpty() && host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0
        && host.compare(m_localHostName, Qt::CaseInsensitive) != 0 && url.path().startsWith(QLatin1Char('/'))) {
        return url.path();
    }

    // Shells that do not report OSC 7, such as Debian and Ubuntu's default
    // bash, still show the directory in the window title.
    return m_hasApplicationTitle ? remoteDirectoryFromTitle(m_applicationTitle) : QString{};
}

QUrl Session::remoteFileUrl(const QString &path) const
{
    if (!hasSshInfo()) {
        return {};
    }

    return Kwi::remoteFileUrl(m_sshMetadata->info(), remoteWorkingDirectory(), path);
}

int Session::scrollbackLines() const
{
    return m_scrollbackLines;
}

void Session::setScrollbackLines(int lines)
{
    if (lines < -1 || lines > 100000 || m_scrollbackLines == lines) {
        return;
    }

    m_scrollbackLines = lines;

    if (m_terminal) {
        m_terminal->setScrollbackLines(lines);
    }

    Q_EMIT scrollbackLinesChanged();
    Q_EMIT m_events.historyChanged();
}

int Session::silenceSeconds() const
{
    return m_silenceSeconds;
}

void Session::setSilenceSeconds(int seconds)
{
    if (seconds < 1 || seconds > 3600 || m_silenceSeconds == seconds) {
        return;
    }

    m_silenceSeconds = seconds;

    if (m_silenceTimer.isActive()) {
        m_silenceTimer.start(seconds * 1000);
    }

    Q_EMIT silenceSecondsChanged();
}

bool Session::sendInput(const QString &text)
{
    return send(text.toUtf8());
}

bool Session::pasteApprovedText(const QString &text)
{
    if (!m_terminal || m_readOnly) {
        return false;
    }

    QString clean;
    clean.reserve(text.size());
    QString normalized{text};
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    for (const QChar character : normalized) {
        if (character == QLatin1Char('\n')) {
            clean += QLatin1Char('\r');
        } else if (character == QLatin1Char('\t') || (character.unicode() >= 0x20 && !(character.unicode() >= 0x7f && character.unicode() <= 0x9f))) {
            clean += character;
        }
    }

    const bool accepted{send(m_terminal->encodePaste(clean.toUtf8()))};

    if (accepted) {
        m_terminal->scrollBottom();
        m_terminal->clearSelection();
    }

    return accepted;
}

Session::ProgressState Session::progressState() const
{
    switch (m_progress.state) {
    case GhosttyQt::Progress::State::None:
        return ProgressState::None;
    case GhosttyQt::Progress::State::Normal:
        return ProgressState::Normal;
    case GhosttyQt::Progress::State::Error:
        return ProgressState::Error;
    case GhosttyQt::Progress::State::Indeterminate:
        return ProgressState::Indeterminate;
    case GhosttyQt::Progress::State::Paused:
        return ProgressState::Paused;
    }

    Q_UNREACHABLE();
}

int Session::progressPercent() const
{
    return m_progress.percent;
}

qint64 Session::processId() const
{
    return qMax<qint64>(0, m_process.processId());
}

qint64 Session::foregroundCommandId() const
{
    return m_process.foregroundProcessId();
}

QString Session::foregroundCommandName() const
{
    return m_process.foregroundProcessName();
}

void Session::resize(int columns, int rows, QSize cellPixels)
{
    columns = qBound(2, columns, 1000);
    rows = qBound(1, rows, 500);

    if (!m_terminal || (columns == m_columns && rows == m_rows && cellPixels == m_cellPixels)) {
        return;
    }

    if (!m_terminal->resize(columns, rows, cellPixels)) {
        reportError(i18nd("kwi-terminal", "Could not resize the terminal."));

        return;
    }

    m_columns = columns;
    m_rows = rows;
    m_cellPixels = cellPixels;
    m_process.resize(columns, rows, cellPixels);

    Q_EMIT m_events.historyChanged();
}

bool Session::send(const QByteArray &bytes)
{
    return !m_readOnly && m_process.write(bytes);
}

void Session::feed(const QByteArray &bytes)
{
    if (!bytes.isEmpty() && !isFinished() && !m_closeRequested) {
        if (m_monitorSilence) {
            m_silenceTimer.start(silenceSeconds() * 1000);
        }

        if (m_monitorActivity && (!m_lastActivity.isValid() || m_lastActivity.elapsed() >= 2000)) {
            m_lastActivity.start();

            Q_EMIT activityDetected();
        }
    }

    if (m_terminal && !m_terminal->feed(bytes)) {
        reportError(i18nd("kwi-terminal", "The terminal could not process output."));
    }

    updateProgress();

    Q_EMIT m_events.historyChanged();

    if (m_terminal) {
        if (const auto notification = m_terminal->takeNotification()) {
            Q_EMIT notificationRequested(notification->title, notification->body);
        }
    }
}

bool Session::monitorActivity() const
{
    return m_monitorActivity;
}

void Session::setMonitorActivity(bool enabled)
{
    enabled = enabled && !isFinished() && !m_closeRequested;

    if (m_monitorActivity == enabled) {
        return;
    }

    m_monitorActivity = enabled;
    m_lastActivity.invalidate();

    Q_EMIT monitoringChanged();
}

bool Session::monitorSilence() const
{
    return m_monitorSilence;
}

void Session::setMonitorSilence(bool enabled)
{
    enabled = enabled && !isFinished() && !m_closeRequested;

    if (m_monitorSilence == enabled) {
        return;
    }

    m_monitorSilence = enabled;

    if (enabled && m_started) {
        m_silenceTimer.start(silenceSeconds() * 1000);
    } else {
        m_silenceTimer.stop();
    }

    Q_EMIT monitoringChanged();
}

bool Session::snapshot(GhosttyQt::Frame &frame)
{
    return m_terminal && m_terminal->snapshot(frame);
}

GhosttyQt::Terminal *Session::terminal() const
{
    return m_terminal.get();
}

qreal Session::zoomOffset() const
{
    return m_zoomOffset;
}

void Session::setZoomOffset(qreal offset)
{
    if (!std::isfinite(offset) || m_zoomOffset == offset) {
        return;
    }

    m_zoomOffset = qBound(-255.0, offset, 255.0);

    Q_EMIT m_events.zoomChanged();
}

bool Session::isReadOnly() const
{
    return m_readOnly;
}

void Session::setReadOnly(bool readOnly)
{
    if (m_readOnly == readOnly) {
        return;
    }

    m_readOnly = readOnly;

    Q_EMIT readOnlyChanged();
}

bool Session::isBusy() const
{
    return m_process.isBusy();
}

QString Session::errorString() const
{
    return m_error;
}

QString Session::observedDirectory() const
{
    if (m_terminal) {
        const QUrl url{m_terminal->workingDirectoryUrl()};
        const QString host{url.host()};

        // Ignore remote OSC 7 directories when resolving a local working directory.
        if (url.isLocalFile()
            && (host.isEmpty() || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
                || host.compare(QSysInfo::machineHostName(), Qt::CaseInsensitive) == 0)) {
            const QString path{url.path()};

            if (QDir::isAbsolutePath(path) && QFileInfo(path).isDir()) {
                return path;
            }
        }
    }

    return m_process.workingDirectory();
}

void Session::updateProgress()
{
    const auto progress = m_terminal ? m_terminal->progress() : GhosttyQt::Progress{};

    if (progress.state != m_progress.state || progress.percent != m_progress.percent) {
        m_progress = progress;

        Q_EMIT progressChanged();
    }
}

QString Session::commandOutput() const
{
    if (!m_terminal) {
        return {};
    }

    return m_terminal->commandOutput();
}

void Session::scrollPrompt(int delta)
{
    if (!m_terminal) {
        return;
    }

    m_terminal->scrollPrompt(delta);
}

void Session::clearScrollback()
{
    if (!m_terminal) {
        return;
    }

    m_terminal->clearScrollback();

    Q_EMIT m_events.historyChanged();
}

void Session::resetTerminal()
{
    if (!m_terminal) {
        return;
    }

    m_terminal->reset();
    updateProgress();

    Q_EMIT m_events.historyChanged();
}

QString Session::selectionText() const
{
    if (!m_terminal) {
        return {};
    }

    return m_terminal->selectionText();
}

QString Session::historyText() const
{
    if (!m_terminal) {
        return {};
    }

    return m_terminal->historyText();
}

void Session::setColors(QColor foreground, QColor background, const QVector<QRgb> &palette, QColor boldForeground)
{
    if (!m_terminal || palette.size() != 16) {
        return;
    }

    m_terminal->setColors(foreground, background, palette, boldForeground);
}

void Session::reportError(const QString &message)
{
    m_error = message;

    Q_EMIT errorChanged();
}
}
