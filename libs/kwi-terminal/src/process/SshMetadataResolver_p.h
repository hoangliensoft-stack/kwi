// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessInfo_p.h"
#include "kwiterminal_export.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <optional>

class KProcess;

namespace Kwi
{
class HostCommandQuery;

struct SshMetadata {
    QString userName;
    QString hostName;
    int port{0};
    QString remoteCommand;

    bool operator==(const SshMetadata &) const = default;
};

struct SshConfigurationQuery {
    QStringList arguments;
    QString remoteCommand;
};

[[nodiscard]] KWI_TERMINAL_EXPORT std::optional<SshConfigurationQuery> sshConfigurationQuery(const QStringList &arguments);
[[nodiscard]] KWI_TERMINAL_EXPORT std::optional<SshMetadata> sshMetadata(const QByteArray &configuration, const QString &command);
// Returns an sftp:// URL for path on destination. Relative paths resolve against
// the absolute remote directory, and ~/ against the conventional home directory.
// Returns an empty URL when the path cannot be resolved.
[[nodiscard]] KWI_TERMINAL_EXPORT QUrl remoteFileUrl(const SshMetadata &destination, const QString &directory, const QString &path);

class KWI_TERMINAL_EXPORT SshMetadataResolver final : public QObject
{
    Q_OBJECT

public:
    explicit SshMetadataResolver(QObject *parent = nullptr);
    ~SshMetadataResolver() override;

    void setProcess(const SshProcessInfo &process);
    [[nodiscard]] bool available() const;
    [[nodiscard]] const SshMetadata &info() const;

Q_SIGNALS:
    void changed();

private:
    void startQuery(const SshConfigurationQuery &command);
    void cancel();
    void readOutput(bool standardError);
    void complete(bool success, const QByteArray &output);

    SshProcessInfo m_process;
    SshMetadata m_info;
    QString m_remoteCommand;
    QPointer<KProcess> m_nativeQuery;
    QPointer<HostCommandQuery> m_hostQuery;
    QTimer m_deadline;
    QByteArray m_output;
    qsizetype m_errorBytes{0};
};
}
