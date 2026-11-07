// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "SessionEvents_p.h"
#include "kwiterminal_export.h"
#include "process/PtyProcess.h"
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>
#include <Terminal.h>
#include <functional>
#include <memory>

namespace Kwi
{
class SshMetadataResolver;
class TerminalView;
struct TerminalLaunch;

/*! \class Kwi::Session
 * \internal
 */

/*!
 * \qmltype Session
 * \inqmlmodule org.kde.kwi.terminal
 * \inherits QtObject
 * \brief Owns a terminal process and its contents.
 *
 * Set the launch properties, then call start(). Creating a Session does not
 * start a process. Attach it to a \l Terminal or TerminalView to display its
 * contents and accept input. The session can outlive its view.
 *
 * The launch properties can be changed while the session is inactive, until
 * close() is called. Changes made while \l active or after close() are ignored.
 * A process that exits on its own can be started again in the same session.
 * Destroying the session closes its process.
 */
class KWI_TERMINAL_EXPORT Session : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Session)
    /*!
     * \qmlproperty string Session::executable
     * The executable name or path to launch. An empty string, the default, uses
     * the user's default shell. Arguments belong in \l arguments, not in this string.
     *
     * \sa start()
     */
    Q_PROPERTY(QString executable READ executable WRITE setExecutable NOTIFY launchChanged)
    /*!
     * \qmlproperty list<string> Session::arguments
     * The arguments passed to \l executable, excluding the executable itself.
     * They are passed literally, without shell expansion. The default is an empty list.
     */
    Q_PROPERTY(QStringList arguments READ arguments WRITE setArguments NOTIFY launchChanged)
    /*!
     * \qmlproperty string Session::initialDirectory
     * The absolute local working directory to use at launch. An empty string, the default,
     * uses homeDirectory. This property does not follow later directory changes.
     *
     * \sa workingDirectory
     */
    Q_PROPERTY(QString initialDirectory READ initialDirectory WRITE setInitialDirectory NOTIFY launchChanged)
    /*!
     * \qmlproperty var Session::environmentChanges
     * Changes to the inherited process environment, expressed as an object whose
     * keys are variable names. A string value sets a variable. A null value removes
     * it. Other value types cause start() to fail. Names must be nonempty and
     * cannot contain \c =. Neither names nor values can contain null characters.
     * The default is an empty object.
     *
     * Assign a new object to change this property. These changes apply when the
     * process starts and do not modify the embedding application's environment.
     */
    Q_PROPERTY(QVariantMap environmentChanges READ environmentChanges WRITE setEnvironmentChanges NOTIFY launchChanged)
    /*!
     * \qmlproperty enumeration Session::state
     * \readonly
     * The process lifecycle state.
     *
     * \value Session.NotStarted No process has been started.
     * \value Session.Starting A launch is in progress.
     * \value Session.Running The process has started.
     * \value Session.Closing The process is being closed.
     * \value Session.Exited The process has exited, or the session was closed before launch.
     * \value Session.Failed The terminal could not be initialized, or the process failed to start.
     *
     * \sa active, finished, errorString
     */
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /*!
     * \qmlproperty bool Session::active
     * \readonly
     * Whether \l state is Starting, Running or Closing. A new launch is rejected
     * while this property is true.
     */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    /*!
     * \qmlproperty bool Session::finished
     * \readonly
     * Whether \l state is Exited or Failed. A session that has never been started is
     * not finished, unless initialization failed or close() was called.
     */
    Q_PROPERTY(bool finished READ isFinished NOTIFY exitStatusChanged)
    /*!
     * \qmlproperty int Session::exitCode
     * \readonly
     * The last process exit code. It is reset to zero when a launch is accepted.
     * Use exited() to observe process completion.
     */
    Q_PROPERTY(int exitCode READ exitCode NOTIFY exitStatusChanged)
    /*!
     * \qmlproperty string Session::errorString
     * \readonly
     * A translated description of the last error, or an empty string if none has
     * been reported. It is cleared when a launch is accepted. An error does not
     * necessarily end the process or change \l state.
     */
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    /*!
     * \qmlproperty bool Session::hasApplicationTitle
     * \readonly
     * Whether a terminal title has been set since the last launch. The title
     * can be empty, including after resetTerminal().
     */
    Q_PROPERTY(bool hasApplicationTitle READ hasApplicationTitle NOTIFY applicationTitleChanged)
    /*!
     * \qmlproperty string Session::applicationTitle
     * \readonly
     * The title supplied by the terminal process, or an empty string before it
     * supplies one. Control characters are replaced with spaces and the title is
     * limited to 256 UTF-16 code units without splitting a surrogate pair.
     *
     * \sa hasApplicationTitle, SessionTitleFormatter
     */
    Q_PROPERTY(QString applicationTitle READ applicationTitle NOTIFY applicationTitleChanged)
    /*!
     * \qmlproperty string Session::launchedProgramName
     * \readonly
     * The launched executable's filename, without its directory. Before the
     * executable is known, this is the translated label \e Terminal.
     *
     * \sa foregroundCommandName
     */
    Q_PROPERTY(QString launchedProgramName READ launchedProgramName NOTIFY launchedProgramNameChanged)
    /*!
     * \qmlproperty string Session::workingDirectory
     * \readonly
     * The last known local working directory. It is initially empty and is set to
     * the launch directory when start() accepts a launch. Later updates use shell
     * integration and process information when available. Remote directories
     * reported by an SSH session do not replace this local path.
     */
    Q_PROPERTY(QString workingDirectory READ workingDirectory NOTIFY workingDirectoryChanged)
    /*!
     * \qmlproperty string Session::localUserName
     * \readonly
     * The local login name, captured when the session is created.
     */
    Q_PROPERTY(QString localUserName READ localUserName CONSTANT)
    /*!
     * \qmlproperty string Session::localHostName
     * \readonly
     * The local machine's hostname, captured when the session is created.
     */
    Q_PROPERTY(QString localHostName READ localHostName CONSTANT)
    /*!
     * \qmlproperty string Session::homeDirectory
     * \readonly
     * The local user's home directory, captured when the session is created.
     */
    Q_PROPERTY(QString homeDirectory READ homeDirectory CONSTANT)
    /*!
     * \qmlproperty bool Session::hasSshInfo
     * \readonly
     * Whether SSH connection information is available for the observed process.
     * It is resolved asynchronously from the local SSH client and its configuration.
     * It can be false while resolution is pending or when the client cannot be inspected.
     */
    Q_PROPERTY(bool hasSshInfo READ hasSshInfo NOTIFY sshInfoChanged)
    /*!
     * \qmlproperty string Session::sshUserName
     * \readonly
     * The SSH destination's resolved user name, or an empty string when
     * hasSshInfo is false.
     */
    Q_PROPERTY(QString sshUserName READ sshUserName NOTIFY sshInfoChanged)
    /*!
     * \qmlproperty string Session::sshHostName
     * \readonly
     * The SSH destination's resolved hostname, without its port, or an empty
     * string when hasSshInfo is false.
     */
    Q_PROPERTY(QString sshHostName READ sshHostName NOTIFY sshInfoChanged)
    /*!
     * \qmlproperty int Session::sshPort
     * \readonly
     * The SSH destination's resolved port, or zero when hasSshInfo is false.
     */
    Q_PROPERTY(int sshPort READ sshPort NOTIFY sshInfoChanged)
    /*!
     * \qmlproperty string Session::sshRemoteCommand
     * \readonly
     * The command requested through SSH, or an empty string for an interactive
     * shell or when hasSshInfo is false. This does not track commands later run
     * inside the remote shell.
     */
    Q_PROPERTY(QString sshRemoteCommand READ sshRemoteCommand NOTIFY sshInfoChanged)
    /*!
     * \qmlproperty string Session::remoteWorkingDirectory
     * \readonly
     * The SSH server's working directory, as reported by its shell through
     * OSC 7, or else read from a "user@host: directory" window title as set
     * by Debian and Ubuntu's default shell configuration. It is empty when
     * hasSshInfo is false or the remote shell reports neither.
     *
     * \sa remoteFileUrl()
     */
    Q_PROPERTY(QString remoteWorkingDirectory READ remoteWorkingDirectory NOTIFY remoteWorkingDirectoryChanged)
    /*!
     * \qmlproperty int Session::processId
     * \readonly
     * The local process ID, or zero when unavailable. In a sandbox, this identifies
     * the process on the host.
     */
    Q_PROPERTY(qint64 processId READ processId NOTIFY processInfoChanged)
    /*!
     * \qmlproperty int Session::foregroundCommandId
     * \readonly
     * The observed local foreground command's process ID, excluding an idle shell.
     * It is -1 when unavailable.
     *
     * \sa foregroundCommandName, busy, refreshProcessInfo()
     */
    Q_PROPERTY(qint64 foregroundCommandId READ foregroundCommandId NOTIFY foregroundCommandChanged)
    /*!
     * \qmlproperty string Session::foregroundCommandName
     * \readonly
     * The observed local foreground command's name, excluding an idle shell.
     * It is empty when unavailable. For SSH, this identifies the local client,
     * not a command running inside the remote shell.
     */
    Q_PROPERTY(QString foregroundCommandName READ foregroundCommandName NOTIFY foregroundCommandChanged)
    /*!
     * \qmlproperty bool Session::busy
     * \readonly
     * Whether closing the session could interrupt a command. An idle shell is not
     * busy. Missing process information is treated as busy while the process is
     * running.
     *
     * \sa refreshProcessInfo()
     */
    Q_PROPERTY(bool busy READ isBusy NOTIFY processInfoChanged)
    /*!
     * \qmlproperty bool Session::readOnly
     * Whether user input is blocked. The default is false. Output, selection and
     * terminal protocol replies continue to work.
     */
    Q_PROPERTY(bool readOnly READ isReadOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    /*!
     * \qmlproperty int Session::scrollbackLines
     * The history limit in lines. Zero disables history. The default, -1, removes
     * the line limit and enables idle compression, with a 150 MB budget for
     * uncompressed terminal pages. Unlimited history can still discard old output
     * when that budget is reached.
     *
     * Values from -1 to 100000 are accepted. Other values are ignored. Reducing
     * the limit can discard existing history.
     */
    Q_PROPERTY(int scrollbackLines READ scrollbackLines WRITE setScrollbackLines NOTIFY scrollbackLinesChanged)
    /*!
     * \qmlproperty bool Session::monitorActivity
     * Whether terminal output emits activityDetected(). The default is false.
     * Monitoring stops when the session finishes or close() is called.
     */
    Q_PROPERTY(bool monitorActivity READ monitorActivity WRITE setMonitorActivity NOTIFY monitoringChanged)
    /*!
     * \qmlproperty bool Session::monitorSilence
     * Whether a period without output emits silenceDetected(). The default is
     * false. Monitoring stops when the session finishes or close() is called.
     *
     * \sa silenceSeconds
     */
    Q_PROPERTY(bool monitorSilence READ monitorSilence WRITE setMonitorSilence NOTIFY monitoringChanged)
    /*!
     * \qmlproperty int Session::silenceSeconds
     * The period without output required for silenceDetected(), in seconds.
     * The default is 10. Values from 1 to 3600 are accepted. Other values are
     * ignored. Changing this property restarts an active silence countdown.
     */
    Q_PROPERTY(int silenceSeconds READ silenceSeconds WRITE setSilenceSeconds NOTIFY silenceSecondsChanged)
    /*!
     * \qmlproperty enumeration Session::progressState
     * \readonly
     * The progress state reported by the terminal process.
     *
     * \value Session.None No progress is being reported.
     * \value Session.Normal Work is progressing.
     * \value Session.Error The process reports an error.
     * \value Session.Indeterminate Work is in progress without a known percentage.
     * \value Session.Paused Work is paused.
     *
     * \sa progressPercent
     */
    Q_PROPERTY(ProgressState progressState READ progressState NOTIFY progressChanged)
    /*!
     * \qmlproperty int Session::progressPercent
     * \readonly
     * The progress percentage reported by the process, from 0 to 100, or -1
     * when unknown. Interpret it together with progressState.
     */
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged)

public:
    enum class State {
        NotStarted,
        Starting,
        Running,
        Closing,
        Exited,
        Failed
    };
    Q_ENUM(State)

    enum class ProgressState {
        None,
        Normal,
        Error,
        Indeterminate,
        Paused
    };
    Q_ENUM(ProgressState)

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    [[nodiscard]] QString executable() const;
    void setExecutable(const QString &newExecutable);
    [[nodiscard]] QStringList arguments() const;
    void setArguments(const QStringList &newArguments);
    [[nodiscard]] QString initialDirectory() const;
    void setInitialDirectory(const QString &directory);
    [[nodiscard]] QVariantMap environmentChanges() const;
    void setEnvironmentChanges(const QVariantMap &changes);

    /*!
     * \qmlmethod bool Session::start()
     * Starts the configured process. Returns true if the launch was accepted.
     * The process may still fail to start asynchronously. Observe started(),
     * exited(), \l state and errorString for the result.
     *
     * Returns false if the session is \l active, was explicitly closed, has no valid
     * terminal, or the launch configuration is rejected. Calling start() never
     * replaces a running process.
     *
     * \sa executable, arguments, initialDirectory, environmentChanges
     */
    Q_INVOKABLE bool start();
    /*!
     * \qmlmethod void Session::close()
     * Closes the process asynchronously and prevents future launches. Repeated
     * calls have no effect. Terminal contents remain available until the session
     * is destroyed or reset.
     *
     * This method does not ask for confirmation. The embedding application should
     * check \l busy and obtain any required confirmation before calling it.
     *
     * \sa exited()
     */
    Q_INVOKABLE void close();

    [[nodiscard]] State state() const;
    [[nodiscard]] bool isActive() const;
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] int exitCode() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString applicationTitle() const;
    [[nodiscard]] bool hasApplicationTitle() const;
    [[nodiscard]] QString launchedProgramName() const;
    [[nodiscard]] QString workingDirectory() const;
    [[nodiscard]] QString localUserName() const;
    [[nodiscard]] QString localHostName() const;
    [[nodiscard]] QString homeDirectory() const;
    [[nodiscard]] bool hasSshInfo() const;
    [[nodiscard]] QString sshUserName() const;
    [[nodiscard]] QString sshHostName() const;
    [[nodiscard]] int sshPort() const;
    [[nodiscard]] QString sshRemoteCommand() const;
    [[nodiscard]] QString remoteWorkingDirectory() const;
    /*!
     * \qmlmethod url Session::remoteFileUrl(string path)
     * Returns an sftp:// URL for \a path on the SSH server, which local editors
     * can open and save in place. Relative paths resolve against
     * remoteWorkingDirectory. Returns an empty URL when hasSshInfo is false or
     * the path cannot be resolved.
     */
    [[nodiscard]] Q_INVOKABLE QUrl remoteFileUrl(const QString &path) const;
    [[nodiscard]] qint64 processId() const;
    [[nodiscard]] qint64 foregroundCommandId() const;
    [[nodiscard]] QString foregroundCommandName() const;
    [[nodiscard]] bool isBusy() const;
    /*!
     * \qmlmethod void Session::refreshProcessInfo()
     * Requests fresh process information. processInfoRefreshed() is emitted when
     * the request completes, including when no new information could be obtained.
     * The request may complete asynchronously.
     */
    Q_INVOKABLE void refreshProcessInfo();

    [[nodiscard]] bool isReadOnly() const;
    void setReadOnly(bool readOnly);
    [[nodiscard]] int scrollbackLines() const;
    void setScrollbackLines(int lines);
    [[nodiscard]] bool monitorActivity() const;
    void setMonitorActivity(bool enabled);
    [[nodiscard]] bool monitorSilence() const;
    void setMonitorSilence(bool enabled);
    [[nodiscard]] int silenceSeconds() const;
    void setSilenceSeconds(int seconds);
    [[nodiscard]] ProgressState progressState() const;
    [[nodiscard]] int progressPercent() const;

    /*!
     * \qmlmethod bool Session::sendInput(string text)
     * Sends \a text as UTF-8 input, including control characters. Returns whether
     * the input was accepted for writing. Returns false in readOnly mode or when
     * there is no writable process.
     *
     * Use this for input supplied by the embedding application. Clipboard text
     * should go through TerminalView::requestPaste() or pasteApprovedText() so
     * paste filtering and bracketed paste are applied.
     */
    Q_INVOKABLE bool sendInput(const QString &text);
    /*!
     * \qmlmethod bool Session::pasteApprovedText(string text)
     * Pastes \a text after any required confirmation has been obtained by the
     * embedding application. Filters control characters, normalizes line endings
     * and honors the process's bracketed paste mode.
     *
     * Returns whether the paste was accepted for writing. Returns false in
     * readOnly mode or when there is no writable process. An accepted paste clears
     * the selection and scrolls to the bottom.
     *
     * \sa TerminalView::requestPaste(), TerminalView::multilinePasteRequested()
     */
    Q_INVOKABLE bool pasteApprovedText(const QString &text);
    /*!
     * \qmlmethod string Session::historyText()
     * Returns the active screen's retained text, including its scrollback.
     * Soft-wrapped lines are joined and trailing whitespace is trimmed.
     * Formatting and images are omitted. Returns an empty string if the text
     * cannot be retrieved.
     *
     * This copies the full retained text synchronously and can be expensive for
     * large histories.
     */
    [[nodiscard]] Q_INVOKABLE QString historyText() const;
    /*!
     * \qmlmethod void Session::scrollPrompt(int delta)
     * Scrolls by \a delta shell prompts. Negative values move to earlier prompts,
     * positive values to later prompts. Requires prompt markers from shell integration.
     */
    Q_INVOKABLE void scrollPrompt(int delta);
    /*!
     * \qmlmethod void Session::clearScrollback()
     * Discards scrollback and clears the selection. The active screen and running
     * process are preserved.
     */
    Q_INVOKABLE void clearScrollback();
    /*!
     * \qmlmethod void Session::resetTerminal()
     * Resets terminal contents and modes. This does not restart the process.
     */
    Q_INVOKABLE void resetTerminal();

    // Private C++ integration.
    [[nodiscard]] SessionEvents *events();
    /** Copy the launch configuration and start it. */
    bool start(const TerminalLaunch &launch, int masterFd = -1);
    bool attachTeletype(int masterFd);
    void refreshProcessInfo(std::function<void()> completed);
    void resize(int columns, int rows, QSize cellPixels = {});
    bool send(const QByteArray &bytes);
    void feed(const QByteArray &bytes);
    bool snapshot(GhosttyQt::Frame &frame);
    [[nodiscard]] GhosttyQt::Terminal *terminal() const;
    [[nodiscard]] QString selectionText() const;
    [[nodiscard]] QString commandOutput() const;
    void setColors(QColor foreground, QColor background, const QVector<QRgb> &palette, QColor boldForeground = {});
    [[nodiscard]] qreal zoomOffset() const;
    void setZoomOffset(qreal offset);

Q_SIGNALS:
    void launchChanged();
    void stateChanged();
    void activeChanged();
    void exitStatusChanged();
    void errorChanged();
    /*!
     * \qmlsignal Session::started()
     * Emitted when the process has started.
     */
    void started();
    /*!
     * \qmlsignal Session::exited(int code)
     * Emitted when the process ends or a launch fails, with exit \a code.
     * The \l state and exitCode properties have been updated before this signal.
     * Closing a session before launch also emits this signal, with code zero.
     */
    void exited(int code);
    void applicationTitleChanged();
    void launchedProgramNameChanged();
    void workingDirectoryChanged(const QString &directory);
    void sshInfoChanged();
    void remoteWorkingDirectoryChanged();
    void processInfoChanged();
    void foregroundCommandChanged();
    /*!
     * \qmlsignal Session::processInfoRefreshed()
     * Emitted when a refreshProcessInfo() request completes. The process
     * properties contain the latest available information.
     */
    void processInfoRefreshed();
    void readOnlyChanged();
    void scrollbackLinesChanged();
    void monitoringChanged();
    void silenceSecondsChanged();
    /*!
     * \qmlsignal Session::activityDetected()
     * Emitted when output arrives while monitorActivity is enabled. During
     * continuous output, emissions are limited to one every two seconds.
     */
    void activityDetected();
    /*!
     * \qmlsignal Session::silenceDetected()
     * Emitted after silenceSeconds without output while monitorSilence is enabled.
     * It is emitted once per quiet period. Further output starts a new countdown.
     */
    void silenceDetected();
    void progressChanged();
    /*!
     * \qmlsignal Session::bell()
     * Emitted when the process requests a terminal bell. The embedding application
     * chooses how to notify the user. \l Terminal provides a visual bell.
     */
    void bell();
    /*!
     * \qmlsignal Session::notificationRequested(string title, string body)
     * Emitted when the process requests a desktop notification with \a title and
     * \a body. The embedding application decides whether and how to display it.
     * Both strings come from terminal output and should be treated as plain text.
     */
    void notificationRequested(const QString &title, const QString &body);

private:
    friend class TerminalView;
    QPointer<TerminalView> m_view;

    void setState(State newState);
    void finish(int code);
    void updateProgress();
    void refreshSessionMetadata();
    void reportError(const QString &message);
    [[nodiscard]] QString observedDirectory() const;

    SessionEvents m_events;
    std::unique_ptr<GhosttyQt::Terminal> m_terminal;
    PtyProcess m_process;
    std::unique_ptr<SshMetadataResolver> m_sshMetadata;
    QString m_executable;
    QStringList m_arguments;
    QString m_initialDirectory;
    QVariantMap m_environmentChanges;
    State m_state{State::NotStarted};
    bool m_started{false};
    bool m_closeRequested{false};
    int m_exitCode{0};
    QString m_error;
    QString m_applicationTitle;
    bool m_hasApplicationTitle{false};
    QString m_launchedProgramName;
    QString m_currentDirectory;
    const QString m_localUserName;
    const QString m_localHostName;
    const QString m_homeDirectory;
    qint64 m_notifiedForegroundId{-1};
    QString m_notifiedForegroundName;
    int m_scrollbackLines{-1};
    QTimer m_silenceTimer;
    QElapsedTimer m_lastActivity;
    bool m_monitorActivity{false};
    bool m_monitorSilence{false};
    int m_silenceSeconds{10};
    bool m_readOnly{false};
    qreal m_zoomOffset{0};
    GhosttyQt::Progress m_progress;
    int m_columns{80};
    int m_rows{24};
    QSize m_cellPixels;
};
}
