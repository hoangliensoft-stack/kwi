// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <Frame.h>
#include <QString>

#include <QColor>
#include <QFont>
#include <QInputMethodEvent>
#include <QList>
#include <QRectF>
#include <QRegion>
#include <QSGNode>
#include <memory>

class QQuickWindow;

namespace GhosttyQt::Quick
{
class TerminalNode final : public QSGNode
{
public:
    struct Options {
        QRectF rect;
        QRectF contentRect;
        QFont font;
        qreal devicePixelRatio{1};
        qreal logicalDpi{96};
        qreal backgroundOpacity{1};
        bool focused{false};
        bool cursorBlinkVisible{true};
        bool textBlinkVisible{true};
        QRegion searchMatches; // Viewport cell coordinates.
        QRegion hoveredLink; // Viewport cell coordinates.
        QColor searchForeground;
        QColor searchBackground;
        QList<TextHighlight> textHighlights; // Sorted by row.
        QString preedit;
        QList<QInputMethodEvent::Attribute> preeditAttributes;
    };

    struct Statistics {
        quint64 frameGeneration{0};
        quint64 shapedRuns{0};
        quint64 rasterizedGlyphs{0};
        quint64 textureUploadBytes{0};
        quint64 rebuiltRows{0};
        quint64 geometryBytes{0};
        int atlasPages{0};
        int cachedRuns{0};
        qsizetype atlasBytes{0};
        qint64 prepareNanoseconds{0};
    };

    explicit TerminalNode(QQuickWindow *window, bool collectStatistics = false);
    ~TerminalNode() override;

    void synchronize(const Frame &frame, const Options &options);
    [[nodiscard]] Statistics statistics() const;
    void preprocess() override;

private:
    void prepare(const Frame &frame, const Options &options);

    class Private;
    std::unique_ptr<Private> d;
};
}
