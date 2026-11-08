// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "TextHighlighter_p.h"

#include <QTest>

using namespace Kwi;
using namespace Qt::StringLiterals;

class TextHighlighterTest : public QObject
{
    Q_OBJECT

private:
    static QStringList tokens(const QString &line)
    {
        QStringList result;

        for (const auto &span : textHighlightSpans(line)) {
            result += line.mid(span.start, span.length);
        }

        return result;
    }

private Q_SLOTS:
    void recognizesTokens_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("log levels") << u"[ERROR] x [Warning] y INFO debug ok"_s << QStringList{u"ERROR"_s, u"Warning"_s, u"INFO"_s, u"debug"_s, u"ok"_s};
        QTest::newRow("url wins over keyword") << u"see https://example.org/error?x=1."_s << QStringList{u"https://example.org/error?x=1"_s};
        QTest::newRow("path wins over keyword") << u"open /var/log/error.log failed"_s << QStringList{u"/var/log/error.log"_s, u"failed"_s};
        QTest::newRow("home and relative paths") << u"~/src and ./build/x"_s << QStringList{u"~/src"_s, u"./build/x"_s};
        QTest::newRow("not a path") << u"and/or N/A"_s << QStringList{};
        QTest::newRow("timestamp") << u"2026-11-07T10:20:30.123Z start"_s << QStringList{u"2026-11-07T10:20:30.123Z"_s};
        QTest::newRow("time") << u"at 10:20:30 now"_s << QStringList{u"10:20:30"_s};
        QTest::newRow("ip with port") << u"connect 192.168.1.10:8080"_s << QStringList{u"192.168.1.10:8080"_s};
        QTest::newRow("uuid") << u"id=123e4567-e89b-12d3-a456-426614174000"_s << QStringList{u"123e4567-e89b-12d3-a456-426614174000"_s};
        QTest::newRow("commit") << u"c45a707 App: fix"_s << QStringList{u"c45a707"_s};
        QTest::newRow("words are not hashes") << u"deadbeef facade"_s << QStringList{};
        QTest::newRow("hex") << u"addr 0x7ffe12"_s << QStringList{u"0x7ffe12"_s};
        QTest::newRow("quantities") << u"took 12 ms, 3.5s, 85% of 2 GiB"_s << QStringList{u"12 ms"_s, u"3.5s"_s, u"85%"_s, u"2 GiB"_s};
        QTest::newRow("key value numbers") << u"size=1024 retries=-1"_s << QStringList{u"1024"_s, u"-1"_s};
        QTest::newRow("bare numbers") << u"thread-19 1193175759: line 1 v1.2.3"_s << QStringList{};
        QTest::newRow("stack frame") << u"        at java.base/java.lang.Thread.run(Thread.java:835)"_s
                                     << QStringList{u"        at"_s, u"java.base/java.lang."_s, u"Thread.java:835"_s};
        QTest::newRow("lambda frame") << u"\tat vn.onepay.Handler.lambda$get$8(Handler.java:190)"_s
                                      << QStringList{u"\tat"_s, u"vn.onepay."_s, u"Handler.java:190"_s};
        QTest::newRow("omitted frames") << u"        ... 24 more"_s << QStringList{u"        ... 24 more"_s};
        QTest::newRow("caused by") << u"Caused by: java.sql.SQLException: ORA-06550: line 1"_s
                                   << QStringList{u"Caused by:"_s, u"java.sql.SQLException"_s, u"ORA-06550"_s};
        QTest::newRow("database codes") << u"PLS-00201: identifier must be declared"_s << QStringList{u"PLS-00201"_s};
        QTest::newRow("exception names need a suffix") << u"Error in MyErrorHandler"_s << QStringList{u"Error"_s};
        QTest::newRow("no numbers inside words") << u"utf8 x86_64 file2.txt"_s << QStringList{};
        QTest::newRow("json payload") << u"Payload: {\"ts\" : 17, \"ok\": true, \"d\": {\"t\": -19.8e2, \"at\": \"2026-11-08 /x error\", \"n\": null}, \"a\": [1, \"b\"]}"_s
                                      << QStringList{u"\"ts\""_s, u"17"_s, u"\"ok\""_s, u"true"_s, u"\"d\""_s, u"\"t\""_s, u"-19.8e2"_s,
                                                     u"\"at\""_s, u"\"2026-11-08 /x error\""_s, u"\"n\""_s, u"null"_s, u"\"a\""_s, u"1"_s, u"\"b\""_s};
        QTest::newRow("escaped quote in json") << u"{\"k\": \"a\\\"b\"}"_s << QStringList{u"\"k\""_s, u"\"a\\\"b\""_s};
        QTest::newRow("not json") << u"wsp[28994]: [DB-Listen] {a: 1} {\"k\": 1"_s << QStringList{};
        QTest::newRow("keywords need boundaries") << u"stderr okay errored"_s << QStringList{};
    }

    void recognizesTokens()
    {
        QFETCH(QString, line);
        QFETCH(QStringList, expected);

        QCOMPARE(tokens(line), expected);
    }

    void spansAreSortedAndDisjoint()
    {
        const QString line{u"2026-01-01 12:00:00 ERROR 10.0.0.1 /tmp/a 42 https://x.org"_s};
        const auto spans = textHighlightSpans(line);
        QVERIFY(!spans.isEmpty());

        for (qsizetype i{1}; i < spans.size(); ++i) {
            QVERIFY(spans[i - 1].start + spans[i - 1].length <= spans[i].start);
        }
    }

    void colorsJson()
    {
        const auto spans = textHighlightSpans(u"{\"k\": \"v\", \"n\": 1, \"b\": false}"_s);
        QCOMPARE(spans.size(), 6);
        QCOMPARE(spans[0].color, 4);
        QVERIFY(spans[0].bold);
        QCOMPARE(spans[1].color, 2);
        QCOMPARE(spans[3].color, 3);
        QCOMPARE(spans[5].color, 5);
    }

    void assignsPaletteColors()
    {
        const auto spans = textHighlightSpans(u"ERROR WARN OK"_s);
        QCOMPARE(spans.size(), 3);
        QCOMPARE(spans[0].color, 1);
        QCOMPARE(spans[1].color, 3);
        QCOMPARE(spans[2].color, 2);
        QVERIFY(spans[0].bold);
    }

    void dimsStackFrameBoilerplate()
    {
        const auto spans = textHighlightSpans(u"    at a.b.C.d(C.java:1)"_s);
        QCOMPARE(spans.size(), 3);
        QCOMPARE(spans[0].color, TextHighlightSpan::DefaultColor);
        QVERIFY(spans[0].faint);
        QVERIFY(spans[1].faint);
        QVERIFY(!spans[2].faint);
    }
};

QTEST_GUILESS_MAIN(TextHighlighterTest)

#include "texthighlightertest.moc"
