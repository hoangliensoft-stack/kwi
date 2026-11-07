// SPDX-FileCopyrightText: 2026 hungld <hungld@onepay.vn>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "process/SshMetadataResolver_p.h"

#include <QTest>

using namespace Kwi;

class RemoteFileUrlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void resolvesPaths_data();
    void resolvesPaths();
    void includesNonDefaultPort();
    void bracketsIpv6Hosts();
    void rejectsMissingHost();
    void readsDirectoryFromTitle_data();
    void readsDirectoryFromTitle();
};

void RemoteFileUrlTest::readsDirectoryFromTitle_data()
{
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("expected");

    QTest::newRow("absolute") << "zk@server: /opt/frp_0.58.0_linux_amd64" << "/opt/frp_0.58.0_linux_amd64";
    QTest::newRow("home") << "zk@server: ~" << "/home/zk";
    QTest::newRow("below home") << "zk@server: ~/projects/web app" << "/home/zk/projects/web app";
    QTest::newRow("root home") << "root@server: ~/.ssh" << "/root/.ssh";
    QTest::newRow("chroot prefix") << "(jail)deploy@build-01.example.org: /srv" << "/srv";
    QTest::newRow("program title") << "vim nginx.conf" << QString();
    QTest::newRow("no space after colon") << "zk@server:/opt" << QString();
    QTest::newRow("relative") << "zk@server: opt" << QString();
    QTest::newRow("other user home") << "zk@server: ~alice" << QString();
}

void RemoteFileUrlTest::readsDirectoryFromTitle()
{
    QFETCH(QString, title);
    QFETCH(QString, expected);

    QCOMPARE(remoteDirectoryFromTitle(title), expected);
}

void RemoteFileUrlTest::resolvesPaths_data()
{
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("directory");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("expected");

    QTest::newRow("absolute") << "deploy" << QString() << "/etc/nginx/nginx.conf" << "sftp://deploy@server.example.org/etc/nginx/nginx.conf";
    QTest::newRow("absolute ignores directory") << "deploy" << "/srv" << "/etc/hosts" << "sftp://deploy@server.example.org/etc/hosts";
    QTest::newRow("relative") << "deploy" << "/var/www/app" << "config/app.yml" << "sftp://deploy@server.example.org/var/www/app/config/app.yml";
    QTest::newRow("parent directory") << "deploy" << "/var/www/app" << "../shared/.env" << "sftp://deploy@server.example.org/var/www/shared/.env";
    QTest::newRow("relative without directory") << "deploy" << QString() << "app.yml" << QString();
    QTest::newRow("relative with relative directory") << "deploy" << "www" << "app.yml" << QString();
    QTest::newRow("home") << "deploy" << QString() << "~/.bashrc" << "sftp://deploy@server.example.org/home/deploy/.bashrc";
    QTest::newRow("root home") << "root" << QString() << "~/.bashrc" << "sftp://root@server.example.org/root/.bashrc";
    QTest::newRow("other user home") << "deploy" << QString() << "~alice/notes.txt" << QString();
    QTest::newRow("spaces are encoded") << "deploy" << QString() << "/srv/my notes.txt" << "sftp://deploy@server.example.org/srv/my%20notes.txt";
    QTest::newRow("hash is part of the path") << "deploy" << QString() << "/srv/a#b.txt" << "sftp://deploy@server.example.org/srv/a%23b.txt";
}

void RemoteFileUrlTest::resolvesPaths()
{
    QFETCH(QString, user);
    QFETCH(QString, directory);
    QFETCH(QString, path);
    QFETCH(QString, expected);

    const QUrl url{remoteFileUrl({user, QStringLiteral("server.example.org"), 22, {}}, directory, path)};

    QCOMPARE(url.toString(QUrl::FullyEncoded), expected);
}

void RemoteFileUrlTest::includesNonDefaultPort()
{
    const QUrl url{remoteFileUrl({QStringLiteral("deploy"), QStringLiteral("server.example.org"), 2222, {}}, {}, QStringLiteral("/etc/hosts"))};

    QCOMPARE(url.toString(QUrl::FullyEncoded), QStringLiteral("sftp://deploy@server.example.org:2222/etc/hosts"));
}

void RemoteFileUrlTest::bracketsIpv6Hosts()
{
    const QUrl url{remoteFileUrl({QStringLiteral("deploy"), QStringLiteral("2001:db8::1"), 22, {}}, {}, QStringLiteral("/etc/hosts"))};

    QCOMPARE(url.toString(QUrl::FullyEncoded), QStringLiteral("sftp://deploy@[2001:db8::1]/etc/hosts"));
}

void RemoteFileUrlTest::rejectsMissingHost()
{
    QVERIFY(remoteFileUrl({QStringLiteral("deploy"), {}, 22, {}}, {}, QStringLiteral("/etc/hosts")).isEmpty());
    QVERIFY(remoteFileUrl({QStringLiteral("deploy"), QStringLiteral("server.example.org"), 22, {}}, {}, {}).isEmpty());
}

QTEST_GUILESS_MAIN(RemoteFileUrlTest)

#include "remotefileurltest.moc"
