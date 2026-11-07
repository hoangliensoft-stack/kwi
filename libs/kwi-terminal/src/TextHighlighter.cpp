// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "TextHighlighter_p.h"

#include <QBitArray>
#include <QRegularExpression>
#include <algorithm>

namespace Kwi
{
namespace
{
enum AnsiColor {
    DefaultColor = TextHighlightSpan::DefaultColor,
    Red = 1,
    Green = 2,
    Yellow = 3,
    Blue = 4,
    Magenta = 5,
    Cyan = 6,
};

struct Rule {
    QRegularExpression expression;
    int color;
    bool bold;
    bool faint{false};
};

QRegularExpression expression(const char *pattern, bool caseInsensitive = false)
{
    QRegularExpression result(QString::fromLatin1(pattern),
                              caseInsensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    result.optimize();

    return result;
}

const QList<Rule> &rules()
{
    // Earlier rules win where matches overlap, so structured tokens come before
    // the keywords and numbers they can contain.
    static const QList<Rule> rules{
        {expression(R"(\b(?:https?|ftp|file)://[^\s<>"'`]*[^\s<>"'`.,;:!?)\]}])", true), Cyan, false},

        // Java stack traces: dim the frame boilerplate so class, method and
        // source location stand out, and emphasize the causes.
        {expression(R"(^\s+at(?= ))"), DefaultColor, false, true},
        {expression(R"(^\s+\.\.\. \d+ (?:more|common frames omitted)\b)"), DefaultColor, false, true},
        {expression(R"((?<=\bat )(?:[\w$]+[./])+(?=[A-Z_$][\w$]*\.[\w$<>]+\())"), DefaultColor, false, true},
        {expression(R"(^\s*(?:Caused by|Suppressed):)"), Red, true},
        {expression(R"(\b[\w$-]+\.(?:java|kt|kts|scala|groovy):\d+\b)"), Blue, false},
        {expression(R"(\b(?:[a-z_$][\w$]*\.)*[A-Z][\w$]*(?:Exception|Error|Throwable)\b)"), Red, true},

        // Database error codes, such as ORA-06550 and PLS-00201.
        {expression(R"(\b(?:ORA|PLS|TNS|SP2|SQL|IMP|EXP|RMAN|DRG|LRM)-\d{4,5}\b)"), Red, true},

        {expression(R"((?<![\w/.~-])(?:~|\.{1,2})?(?:/[\w.@+-]+)+/?)"), Blue, false},
        {expression(R"(\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b)", true), Magenta, false},
        {expression(R"(\b\d{4}-\d{2}-\d{2}(?:[T ]\d{2}:\d{2}(?::\d{2}(?:[.,]\d+)?)?(?:Z|[+-]\d{2}:?\d{2})?)?\b|\b\d{2}:\d{2}:\d{2}(?:[.,]\d+)?\b)"),
         Magenta,
         false},
        {expression(R"(\b(?:\d{1,3}\.){3}\d{1,3}(?:/\d{1,2}|:\d{1,5})?\b)"), Cyan, false},
        {expression(R"(\b[0-9a-f]{2}(?::[0-9a-f]{2}){5}\b)", true), Cyan, false},
        {expression(R"(\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*\d)[0-9a-f]{7,64}\b)"), Magenta, false},
        {expression(R"(\b0x[0-9a-f]+\b)", true), Yellow, false},
        {expression(R"(\b(?:errors?|err|fatal|fail(?:ed|ure|s)?|critical|crit|panic|exception|denied|refused)\b)", true), Red, true},
        {expression(R"(\b(?:warn(?:ings?)?|deprecated)\b)", true), Yellow, true},
        {expression(R"(\b(?:ok|success(?:ful(?:ly)?)?|succeeded|pass(?:ed)?|done)\b)", true), Green, true},
        {expression(R"(\b(?:info|notice|debug|trace)\b)", true), Blue, true},
        // Bare numbers are too common in logs to be useful. Only highlight
        // quantities with a unit and key=value numbers.
        {expression(R"((?<![\w.])\d+(?:\.\d+)?(?:\s?(?:ns|us|ms|s|sec|secs|seconds|min|mins|h|[KMGT]i?B|B|bytes)\b|%))"), Yellow, false},
        {expression(R"((?<==)-?\d+(?:\.\d+)?\b)"), Yellow, false},
    };

    return rules;
}
}

QList<TextHighlightSpan> textHighlightSpans(QStringView line)
{
    QList<TextHighlightSpan> spans;
    QBitArray claimed;

    for (const auto &rule : rules()) {
        auto matches = rule.expression.globalMatchView(line);

        while (matches.hasNext()) {
            const auto match = matches.next();
            const qsizetype start{match.capturedStart()};
            const qsizetype length{match.capturedLength()};

            if (!length) {
                continue;
            }

            if (claimed.isEmpty()) {
                claimed.resize(line.size());
            }

            bool overlaps{false};

            for (qsizetype i{start}; i < start + length && !overlaps; ++i) {
                overlaps = claimed.testBit(i);
            }

            if (overlaps) {
                continue;
            }

            claimed.fill(true, start, start + length);
            spans += {start, length, rule.color, rule.bold, rule.faint};
        }
    }

    std::ranges::sort(spans, {}, &TextHighlightSpan::start);

    return spans;
}
}
