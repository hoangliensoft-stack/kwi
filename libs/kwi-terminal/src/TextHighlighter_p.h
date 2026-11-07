// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "kwiterminal_export.h"
#include <QList>
#include <QStringView>

namespace Kwi
{
struct TextHighlightSpan {
    static constexpr int DefaultColor{-1};

    qsizetype start{0};
    qsizetype length{0};
    int color{0}; // ANSI palette index, or DefaultColor to keep the default foreground.
    bool bold{false};
    bool faint{false};

    bool operator==(const TextHighlightSpan &) const = default;
};

// Returns non-overlapping spans of recognized tokens in a logical line, sorted by start.
[[nodiscard]] KWI_TERMINAL_EXPORT QList<TextHighlightSpan> textHighlightSpans(QStringView line);
}
