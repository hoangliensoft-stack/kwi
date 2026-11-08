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

// Recognizes JSON objects and arrays embedded in a line, such as log payloads.
class JsonScanner
{
public:
    explicit JsonScanner(QStringView text)
        : m_text{text}
    {
    }

    // Parses a value starting at \a start. On success returns the end offset
    // and appends its token spans.
    qsizetype scan(qsizetype start, QList<TextHighlightSpan> &spans)
    {
        m_position = start;
        m_tokens.clear();
        m_hasString = false;

        if (!value(0) || !m_hasString) {
            return -1;
        }

        spans += m_tokens;

        return m_position;
    }

private:
    static constexpr int MaximumDepth{64};

    QChar peek() const
    {
        return m_position < m_text.size() ? m_text[m_position] : QChar{};
    }

    void skipSpace()
    {
        while (m_position < m_text.size() && (m_text[m_position] == u' ' || m_text[m_position] == u'\t')) {
            ++m_position;
        }
    }

    void token(qsizetype start, int color, bool bold = false)
    {
        m_tokens += {start, m_position - start, color, bold, false};
    }

    bool string(bool key)
    {
        const qsizetype start{m_position++};

        while (m_position < m_text.size()) {
            const QChar c{m_text[m_position++]};

            if (c == u'"') {
                token(start, key ? Blue : Green, key);
                m_hasString = true;

                return true;
            }

            if (c == u'\\') {
                if (m_position == m_text.size()) {
                    return false;
                }

                ++m_position;
            } else if (c.unicode() < 0x20) {
                return false;
            }
        }

        return false;
    }

    bool digits()
    {
        const qsizetype start{m_position};

        while (m_position < m_text.size() && m_text[m_position].isDigit() && m_text[m_position].unicode() < 0x80) {
            ++m_position;
        }

        return m_position > start;
    }

    bool number()
    {
        const qsizetype start{m_position};

        if (peek() == u'-') {
            ++m_position;
        }

        if (!digits()) {
            return false;
        }

        if (peek() == u'.') {
            ++m_position;

            if (!digits()) {
                return false;
            }
        }

        if (peek() == u'e' || peek() == u'E') {
            ++m_position;

            if (peek() == u'+' || peek() == u'-') {
                ++m_position;
            }

            if (!digits()) {
                return false;
            }
        }

        token(start, Yellow);

        return true;
    }

    bool literal()
    {
        for (const QStringView word : {QStringView{u"true"}, QStringView{u"false"}, QStringView{u"null"}}) {
            if (m_text.sliced(m_position).startsWith(word)) {
                const qsizetype start{m_position};
                m_position += word.size();
                token(start, Magenta);

                return true;
            }
        }

        return false;
    }

    bool container(int depth, QChar close)
    {
        ++m_position;
        skipSpace();

        if (peek() == close) {
            ++m_position;

            return true;
        }

        while (true) {
            if (close == u'}') {
                if (peek() != u'"' || !string(true)) {
                    return false;
                }

                skipSpace();

                if (peek() != u':') {
                    return false;
                }

                ++m_position;
                skipSpace();
            }

            if (!value(depth + 1)) {
                return false;
            }

            skipSpace();

            if (peek() == close) {
                ++m_position;

                return true;
            }

            if (peek() != u',') {
                return false;
            }

            ++m_position;
            skipSpace();
        }
    }

    bool value(int depth)
    {
        if (depth > MaximumDepth) {
            return false;
        }

        const QChar c{peek()};

        if (c == u'{') {
            return container(depth, u'}');
        }

        if (c == u'[') {
            return container(depth, u']');
        }

        // Only containers start a JSON value in free text.
        if (depth == 0) {
            return false;
        }

        if (c == u'"') {
            return string(false);
        }

        if (c == u'-' || (c.isDigit() && c.unicode() < 0x80)) {
            return number();
        }

        return literal();
    }

    QStringView m_text;
    qsizetype m_position{0};
    QList<TextHighlightSpan> m_tokens;
    bool m_hasString{false};
};
}

QList<TextHighlightSpan> textHighlightSpans(QStringView line)
{
    QList<TextHighlightSpan> spans;
    QBitArray claimed;

    // JSON comes first so that its strings are not split into paths, dates and keywords.
    JsonScanner json{line};

    for (qsizetype i{0}; i < line.size(); ++i) {
        if (line[i] != u'{' && line[i] != u'[') {
            continue;
        }

        const qsizetype end{json.scan(i, spans)};

        if (end < 0) {
            continue;
        }

        if (claimed.isEmpty()) {
            claimed.resize(line.size());
        }

        claimed.fill(true, i, end);
        i = end - 1;
    }

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
