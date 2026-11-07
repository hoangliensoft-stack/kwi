// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HostCommandQuery_p.h"
#include "ProcessQueryLimits_p.h"
#include "SshMetadataResolver_p.h"

#include <KProcess>
#include <KSandbox>
#include <QDBusConnection>
#include <QDir>
#include <QHostAddress>
#include <QProcess>
#include <QStringView>
#include <QThread>
#include <QtConcurrentRun>
#include <csignal>
#include <memory>
#include <utility>

namespace Kwi
{
namespace
{
void killNativeQuery(QProcess &query)
{
    // Match exec may have a child: the query owns a separate session/group.
    if (query.processId() > 0) {
        kill(-pid_t(query.processId()), SIGKILL);
    }

    query.kill();
}
}

std::optional<SshConfigurationQuery> sshConfigurationQuery(const QStringList &arguments)
{
    if (arguments.size() < 2) {
        return std::nullopt;
    }

    qsizetype size{0};

    for (const QString &argument : arguments) {
        size += argument.toLocal8Bit().size() + 1;

        if (argument.contains(QChar::Null) || size > ProcessQueryLimits::MaximumCommandLineBytes) {
            return std::nullopt;
        }
    }

    constexpr QStringView flags{u"1246ACGKMNTVXYZafgknqstvxy"};
    constexpr QStringView values{u"BDEFIJLOPQRSWbceilmopw"};
    SshConfigurationQuery result{{QStringLiteral("-G")}, {}};
    bool destination{false};
    bool options{true};

    for (qsizetype i{1}; i < arguments.size(); ++i) {
        const QString &argument{arguments[i]};

        if (options && argument == QLatin1String("--")) {
            options = false;
            result.arguments.append(argument);

            continue;
        }

        if (options && argument.startsWith(QLatin1Char('-')) && argument.size() > 1) {
            for (qsizetype j{1}; j < argument.size(); ++j) {
                const QChar option{argument[j]};

                if (QStringView(u"GOVQZ").contains(option)) {
                    return std::nullopt;
                }

                if (values.contains(option)) {
                    QString value{argument.mid(j + 1)};

                    if (value.isEmpty()) {
                        if (++i == arguments.size()) {
                            return std::nullopt;
                        }

                        value = arguments[i];
                    }

                    // A configuration query must not append to the client's log.
                    if (option != QLatin1Char('E')) {
                        result.arguments.append(QLatin1Char('-') + QString(option));
                        result.arguments.append(value);
                    }

                    break;
                }

                if (!flags.contains(option)) {
                    return std::nullopt;
                }

                if (option != QLatin1Char('y')) {
                    result.arguments.append(QLatin1Char('-') + QString(option));
                }
            }

            continue;
        }

        if (!destination) {
            if (argument.isEmpty()) {
                return std::nullopt;
            }

            destination = true;
            result.arguments.append(argument);
        } else {
            // OpenSSH parses options again immediately after the destination.
            // After the first command word, every remaining argument is literal.
            const QStringList command{arguments.mid(i)};
            result.remoteCommand = command.join(QLatin1Char(' '));
            result.arguments.append(command);

            break;
        }
    }

    return destination ? std::optional(result) : std::nullopt;
}

std::optional<SshMetadata> sshMetadata(const QByteArray &configuration, const QString &command)
{
    if (configuration.size() > ProcessQueryLimits::MaximumOutputBytes || configuration.contains('\0')) {
        return std::nullopt;
    }

    SshMetadata info;

    const auto lines = configuration.split('\n');

    for (const QByteArray &line : lines) {
        const qsizetype separator{line.indexOf(' ')};
        const auto key = line.left(separator);
        const QString value{QString::fromLocal8Bit(line.mid(separator + 1))};

        if (key == "hostname") {
            info.hostName = value;
        } else if (key == "user") {
            info.userName = value;
        } else if (key == "port") {
            info.port = value.toInt();
        } else if (key == "remotecommand" && value != QLatin1String("none")) {
            info.remoteCommand = value;
        }
    }

    if (info.hostName.isEmpty() || info.userName.isEmpty() || info.port < 1 || info.port > 65535) {
        return std::nullopt;
    }

    const QString addressText{info.hostName.startsWith(QLatin1Char('[')) && info.hostName.endsWith(QLatin1Char(']'))
                                  ? info.hostName.mid(1, info.hostName.size() - 2)
                                  : info.hostName};
    const QHostAddress address{addressText};

    if (!address.isNull()) {
        info.hostName = addressText;
    }

    if (!command.isEmpty()) {
        info.remoteCommand = command;
    }

    return info;
}

QUrl remoteFileUrl(const SshMetadata &destination, const QString &directory, const QString &path)
{
    if (destination.hostName.isEmpty() || path.isEmpty() || path.contains(QChar::Null)) {
        return {};
    }

    QString absolute;

    if (path.startsWith(QLatin1Char('/'))) {
        absolute = path;
    } else if (path == u"~" || path.startsWith(QLatin1String("~/"))) {
        // The server does not report its home directory, so use the common layout.
        if (destination.userName.isEmpty()) {
            return {};
        }

        const QString home{destination.userName == u"root" ? QStringLiteral("/root") : QStringLiteral("/home/") + destination.userName};
        absolute = home + path.mid(1);
    } else if (path.startsWith(QLatin1Char('~'))) {
        // ~user paths need the server's user database.
        return {};
    } else {
        if (!directory.startsWith(QLatin1Char('/'))) {
            return {};
        }

        absolute = directory + QLatin1Char('/') + path;
    }

    QUrl url;
    url.setScheme(QStringLiteral("sftp"));
    url.setUserName(destination.userName);
    url.setHost(destination.hostName);

    if (destination.port > 0 && destination.port != 22) {
        url.setPort(destination.port);
    }

    url.setPath(QDir::cleanPath(absolute));

    return url.isValid() ? url : QUrl{};
}

SshMetadataResolver::SshMetadataResolver(QObject *parent)
    : QObject{parent}
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(ProcessQueryLimits::TimeoutMs);

    connect(&m_deadline, &QTimer::timeout, this, [this] {
        cancel();
    });
}

SshMetadataResolver::~SshMetadataResolver()
{
    cancel();
}

void SshMetadataResolver::setProcess(const SshProcessInfo &process)
{
    if (m_process.pid == process.pid && m_process.startTime == process.startTime && m_process.executable == process.executable
        && m_process.arguments == process.arguments) {
        return;
    }

    cancel();
    m_process = process;

    const bool hadInfo{available()};
    m_info = {};
    const auto command = sshConfigurationQuery(process.arguments);

    if (process.pid > 0 && !process.executable.isEmpty() && command) {
        startQuery(*command);
    }

    // Publish last because a handler can close the Session and cancel the new query.
    if (hadInfo) {
        Q_EMIT changed();
    }
}

void SshMetadataResolver::startQuery(const SshConfigurationQuery &command)
{
    m_remoteCommand = command.remoteCommand;

    if (KSandbox::isFlatpak()) {
        auto *query = new HostCommandQuery(QDBusConnection::sessionBus(), this);
        m_hostQuery = query;

        connect(query, &HostCommandQuery::finished, this, [this, query](HostCommandQuery::Result result, const QByteArray &output) {
            m_hostQuery.clear();
            query->deleteLater();
            complete(result == HostCommandQuery::Result::Success, output);
        });

        query->start(QStringList{m_process.executable} + command.arguments, m_process.directory);

        return;
    }

    auto *query = new KProcess(this);
    m_nativeQuery = query;
    query->setProgram(m_process.executable, command.arguments);
    query->setWorkingDirectory(m_process.directory);
    query->setStandardInputFile(QProcess::nullDevice());
    query->setOutputChannelMode(KProcess::SeparateChannels);
    query->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);

    connect(query, &QProcess::readyReadStandardOutput, this, [this] {
        readOutput(false);
    });

    connect(query, &QProcess::readyReadStandardError, this, [this] {
        readOutput(true);
    });

    connect(query, &QProcess::errorOccurred, this, [this, query](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_deadline.stop();
            m_nativeQuery.clear();
            m_output.clear();
            m_errorBytes = 0;
            query->deleteLater();
        }
    });

    connect(query, &QProcess::finished, this, [this, query](int exitCode, QProcess::ExitStatus status) {
        m_deadline.stop();
        m_nativeQuery.clear();
        query->deleteLater();
        complete(status == QProcess::NormalExit && exitCode == 0, std::exchange(m_output, {}));
    });

    m_deadline.start();
    query->start();
}

bool SshMetadataResolver::available() const
{
    return !m_info.hostName.isEmpty();
}

const SshMetadata &SshMetadataResolver::info() const
{
    return m_info;
}

void SshMetadataResolver::cancel()
{
    m_deadline.stop();
    delete m_hostQuery.data();
    m_output.clear();
    m_errorBytes = 0;

    if (!m_nativeQuery) {
        return;
    }

    std::unique_ptr<KProcess> query{m_nativeQuery.data()};
    m_nativeQuery.clear();
    query->disconnect(this); // clazy:exclude=unused-result-check
    query->setParent(nullptr);

    if (query->state() != QProcess::NotRunning) {
        killNativeQuery(*query);
        query->moveToThread(nullptr);
        (void)QtConcurrent::run([query = std::move(query)]() mutable {
            query->moveToThread(QThread::currentThread());
            query->waitForFinished(-1);
        });
    }
}

void SshMetadataResolver::readOutput(bool standardError)
{
    const QByteArray bytes{standardError ? m_nativeQuery->readAllStandardError() : m_nativeQuery->readAllStandardOutput()};
    const qsizetype previous{standardError ? m_errorBytes : m_output.size()};

    if (bytes.size() > ProcessQueryLimits::MaximumOutputBytes - previous) {
        m_output.clear();
        m_nativeQuery->closeReadChannel(QProcess::StandardOutput);
        m_nativeQuery->closeReadChannel(QProcess::StandardError);
        killNativeQuery(*m_nativeQuery);

        return;
    }

    if (standardError) {
        m_errorBytes += bytes.size();
    } else {
        m_output.append(bytes);
    }
}

void SshMetadataResolver::complete(bool success, const QByteArray &output)
{
    const auto result = success ? sshMetadata(output, m_remoteCommand) : std::nullopt;

    if (result) {
        m_info = *result;
        Q_EMIT changed();
    }
}
}
