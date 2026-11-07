// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <array>

namespace GhosttyQt
{
enum CellFlag : quint16 {
    Bold = 1,
    Italic = 2,
    Underline = 4,
    Strike = 8,
    Faint = 16,
    Invisible = 32,
    Selected = 64,
    Blink = 128,
    Overline = 256,
    Inverse = 512,
    ImagePlaceholder = 1024,
};

struct Cell {
    QString text;
    QRgb foreground{qRgb(255, 255, 255)};
    QRgb background{qRgb(0, 0, 0)};
    QRgb underlineColor{qRgb(255, 255, 255)};
    quint16 flags{0};
    quint8 width{1}; // 0: wide spacer; 1: normal; 2: wide leader.
    quint8 underline{0}; // None, single, double, curly, dotted, dashed.
    bool defaultBackground{true};
    bool defaultForeground{true}; // No SGR foreground color and not inverse.

    bool operator==(const Cell &) const = default;
};

// A foreground override applied while rendering, without changing the terminal contents.
struct TextHighlight {
    QRect cells; // Viewport cell coordinates, a single row.
    QRgb foreground{0};
    bool bold{false};
    bool faint{false};

    bool operator==(const TextHighlight &) const = default;
};

enum class CursorStyle {
    Beam,
    Block,
    Underline,
    HollowBlock
};

enum class ImageLayer {
    BelowBackground,
    BelowText,
    AboveText
};

struct ImagePlacement {
    quint32 imageId{0};
    quint32 placementId{0};
    quint64 generation{0}; // Opaque pixel revision, unique across screen switches and resets.
    ImageLayer layer{ImageLayer::AboveText};
    qint32 z{0};
    QPoint cell;
    QPoint pixelOffset;
    QSize pixelSize;
    QSize gridSize;
    QRect source;
    QImage image;
};

struct Frame {
    quint64 terminalId{0}; // Producer identity. Generations are local to this terminal.
    QVector<QVector<Cell>> rows;
    QVector<bool> rowWrapped;
    QVector<quint64> rowVersions;
    QVector<std::array<quint64, 2>> rowIdentities;
    QVector<ImagePlacement> images; // Paint order: layer, z, imageId, placementId.
    quint64 generation{0};
    quint64 imagesGeneration{0};

    int columns{0};
    QPoint cursor{-1, -1};
    bool cursorVisible{false};
    bool cursorBlinking{false};
    bool cursorWideTail{false};
    CursorStyle cursorStyle{CursorStyle::Block};

    QColor foreground;
    QColor background;
    QColor cursorColor;
    QColor selectionForeground;
    QColor selectionBackground;

    quint64 scrollTotal{0};
    quint64 scrollOffset{0};
};
}
