// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Terminal.h"
#include "HistoryReader.h"
#include "HistoryReader_p.h"
#include "QtInput_p.h"
#include "Search.h"
#include "Search_p.h"

#include <ghostty/vt.h>

#include <QBuffer>
#include <QChar>
#include <QElapsedTimer>
#include <QHash>
#include <QImageReader>
#include <QKeyEvent>
#include <QMimeData>
#include <QSet>
#include <QStringView>
#include <QThread>
#include <QTimer>
#include <QUtf8StringView>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace GhosttyQt
{
namespace
{
template<typename T, auto Free>
class Handle
{
public:
    ~Handle()
    {
        if (value)
            Free(value);
    }

    Handle() = default;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    operator T() const
    {
        return value;
    }

    T *out()
    {
        return &value;
    }

    T value{nullptr};
};

constexpr size_t ImageLimit = 64 * 1024 * 1024;

QString string(GhosttyString value)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(value.ptr), qsizetype(value.len));
}

QString notificationText(GhosttyString value, qsizetype limit)
{
    QString result;
    result.reserve(std::min(qsizetype(value.len), limit));

    const auto text = string(value);

    for (const QChar c : text) {
        const ushort code{c.unicode()};

        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) || (code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069))
            continue;

        if (result.size() >= limit)
            break;

        result.append(c);
    }

    if (!result.isEmpty() && result.back().isHighSurrogate())
        result.chop(1);

    return result;
}

QRgb rgb(GhosttyColorRgb c)
{
    return qRgb(c.r, c.g, c.b);
}

GhosttyColorRgb rgb(QColor c)
{
    return {quint8(c.red()), quint8(c.green()), quint8(c.blue())};
}

GhosttyColorScheme colorScheme(GhosttyColorRgb background)
{
    return ghostty_color_perceived_luminance(&background) > 0.5 ? GHOSTTY_COLOR_SCHEME_LIGHT : GHOSTTY_COLOR_SCHEME_DARK;
}

constexpr CursorStyle cursorStyle(GhosttyRenderStateCursorVisualStyle value)
{
    switch (value) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        return CursorStyle::Beam;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        return CursorStyle::Underline;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        return CursorStyle::HollowBlock;
    default:
        return CursorStyle::Block;
    }
}

constexpr GhosttyTerminalCursorStyle cursorStyle(CursorStyle value)
{
    switch (value) {
    case CursorStyle::Beam:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_BAR;
    case CursorStyle::Underline:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_UNDERLINE;
    case CursorStyle::HollowBlock:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK_HOLLOW;
    case CursorStyle::Block:
        return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
    }

    return GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
}

GhosttyPoint point(QPoint p, GhosttyPointTag tag = GHOSTTY_POINT_TAG_VIEWPORT)
{
    GhosttyPoint result{};
    result.tag = tag;
    result.value.coordinate = {quint16(p.x()), quint32(p.y())};

    return result;
}

// This process-wide decoder may be called from different terminal threads.
bool decodePng(void *, const GhosttyAllocator *allocator, const uint8_t *data, size_t length, GhosttySysImage *out)
{
    static constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    if (!out || length > ImageLimit || length < signature.size() || std::memcmp(data, signature.data(), signature.size()) != 0)
        return false;

    QByteArray bytes{QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(length))};
    QBuffer input{&bytes};
    input.open(QIODevice::ReadOnly);
    QImageReader reader{&input, "png"};
    const QSize size{reader.size()};

    if (size.isEmpty() || quint64(size.width()) * quint64(size.height()) > ImageLimit / 4)
        return false;

    QImage image{reader.read().convertToFormat(QImage::Format_RGBA8888)};

    if (image.isNull() || image.size() != size)
        return false;

    const size_t amount{size_t(image.width()) * size_t(image.height()) * 4};
    auto *memory = ghostty_alloc(allocator, amount);

    if (!memory)
        return false;

    for (int y{0}; y < image.height(); ++y) {
        std::memcpy(memory + size_t(y) * size_t(image.width()) * 4, image.constScanLine(y), size_t(image.width()) * 4);
    }

    *out = {quint32(image.width()), quint32(image.height()), memory, amount};

    return true;
}

QString formattedSelection(GhosttyTerminal terminal, const GhosttySelection *selection)
{
    GhosttyTerminalSelectionFormatOptions options{GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions)};
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    options.selection = selection;
    uint8_t *bytes{nullptr};
    size_t length{0};

    if (ghostty_terminal_selection_format_alloc(terminal, nullptr, options, &bytes, &length) != GHOSTTY_SUCCESS)
        return {};

    const QString result{QString::fromUtf8(reinterpret_cast<const char *>(bytes), qsizetype(length))};
    ghostty_free(nullptr, bytes, length);

    return result;
}

}

class Terminal::Private
{
public:
    explicit Private(Terminal *owner)
        : q{owner}
    {
        holdTimer.setParent(q);
        holdTimer.setSingleShot(true);

        QObject::connect(&holdTimer, &QTimer::timeout, q, &Terminal::endSynchronizedOutput);
        animationTimer.setParent(q);
        animationTimer.setSingleShot(true);

        QObject::connect(&animationTimer, &QTimer::timeout, q, [this] {
            tickImages();

            invalidateFrame();
        });

        compressionTimer.setParent(q);
        compressionTimer.setSingleShot(true);

        QObject::connect(&compressionTimer, &QTimer::timeout, q, [this] {
            if (!q->isValid() || !compressionEnabled || scheduleCompression())
                return;

            GhosttyTerminalCompressionResult result{};

            if (ghostty_terminal_compress(terminal, GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL, &result) == GHOSTTY_SUCCESS
                && result == GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING)
                compressionTimer.start(std::chrono::milliseconds{1});
        });

        clock.start();
    }

    ~Private()
    {
        if (gesture)
            ghostty_selection_gesture_free(gesture, terminal);
    }

    void assertThread() const
    {
        Q_ASSERT(QThread::currentThread() == q->thread());
    }

    bool fail(Error value)
    {
        error = value;

        Q_EMIT q->errorOccurred(error);

        return false;
    }

    bool mode(GhosttyMode value) const
    {
        GhosttyTerminalModeConfig config{value, false};

        return ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE, &config) == GHOSTTY_SUCCESS && config.value;
    }

    GhosttyGridRef ref(QPoint p) const
    {
        GhosttyGridRef result{GHOSTTY_INIT_SIZED(GhosttyGridRef)};

        if (p.x() < 0 || p.y() < 0 || p.x() >= columns || p.y() >= rows)
            return result;

        ghostty_terminal_grid_ref(terminal, point(p), &result);

        return result;
    }

    void resetGesture()
    {
        ghostty_selection_gesture_reset(gesture, terminal);
    }

    bool selectionEquals(const GhosttySelection *selection) const
    {
        GhosttySelection current{GHOSTTY_INIT_SIZED(GhosttySelection)};
        const auto result = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &current);

        if (!selection)
            return result == GHOSTTY_NO_VALUE;

        bool equal{false};

        return result == GHOSTTY_SUCCESS && ghostty_terminal_selection_equal(terminal, &current, selection, &equal) == GHOSTTY_SUCCESS && equal;
    }

    void invalidateFrame()
    {
        contentInvalidated = true;
        scheduleCompression();

        Q_EMIT q->frameChanged();
    }

    bool scheduleCompression()
    {
        if (!compressionEnabled || !q->isValid())
            return false;

        uint64_t activity{0};

        if (ghostty_terminal_compression_activity(terminal, &activity) != GHOSTTY_SUCCESS || compressionActivity == activity)
            return false;

        // Match Ghostty's 250 ms idle delay, restarting only when compression activity changes.
        compressionActivity = activity;
        compressionTimer.start(std::chrono::milliseconds{250});

        return true;
    }

    bool setSelection(const GhosttySelection *selection, bool viewportChanged = false)
    {
        const bool changed{!selectionEquals(selection)};

        if (changed && ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_SELECTION, selection) != GHOSTTY_SUCCESS)
            return false;

        if (viewportChanged)
            invalidateFrame();
        else if (changed)
            Q_EMIT q->frameChanged();

        return true;
    }

    GhosttyKittyGraphics graphics() const
    {
        GhosttyKittyGraphics result{nullptr};
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &result);

        return result;
    }

    void tickImages()
    {
        auto storage = graphics();
        uint64_t next{0};
        const quint64 now{quint64(clock.elapsed())};

        if (storage && ghostty_kitty_graphics_animation_tick(storage, now, &next) == GHOSTTY_SUCCESS) {
            animationTimer.start(int(std::clamp<quint64>(next > now ? next - now : 1, 1, std::numeric_limits<int>::max())));
        } else {
            animationTimer.stop();
        }
    }

    QImage image(quint32 id, quint64 generation)
    {
        auto it = imageCache.constFind(id);

        if (it != imageCache.cend() && it->first == generation)
            return it->second;

        const auto data = ghostty_kitty_graphics_image(graphics(), id);
        uint32_t width{0};
        uint32_t height{0};
        GhosttyKittyImageFormat format{GHOSTTY_KITTY_IMAGE_FORMAT_RGBA};
        const uint8_t *bytes{nullptr};
        size_t length{0};
        const GhosttyKittyGraphicsImageData keys[] = {
            GHOSTTY_KITTY_IMAGE_DATA_WIDTH,
            GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,
            GHOSTTY_KITTY_IMAGE_DATA_FORMAT,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,
        };

        void *values[] = {&width, &height, &format, &bytes, &length};

        if (!data || ghostty_kitty_graphics_image_get_multi(data, std::size(keys), keys, values, nullptr) != GHOSTTY_SUCCESS || width == 0 || height == 0
            || quint64(width) * height > ImageLimit / 4)
            return {};

        const int channels = format == GHOSTTY_KITTY_IMAGE_FORMAT_RGB ? 3
            : format == GHOSTTY_KITTY_IMAGE_FORMAT_RGBA               ? 4
            : format == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA         ? 2
            : format == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY               ? 1
                                                                      : 0;

        if (!channels || quint64(width) * height * channels != length)
            return {};

        QImage result;

        if (channels == 4 || channels == 3) {
            result = QImage(bytes, int(width), int(height), int(width) * channels, channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888).copy();
        } else {
            result = QImage(int(width), int(height), QImage::Format_RGBA8888);

            for (quint32 y = 0; y < height; ++y) {
                auto *target = result.scanLine(int(y));

                for (quint32 x = 0; x < width; ++x) {
                    const auto *source = bytes + (quint64(y) * width + x) * channels;
                    target[4 * x] = target[4 * x + 1] = target[4 * x + 2] = source[0];
                    target[4 * x + 3] = channels == 2 ? source[1] : 255;
                }
            }
        }

        // Keep RGB images at three bytes per pixel to respect the emulator's storage budget.
        if (result.hasAlphaChannel())
            result = result.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

        imageCache.insert(id, {generation, result});

        return result;
    }

    bool capture()
    {
        if (!terminal || ghostty_render_state_update(render, terminal) != GHOSTTY_SUCCESS)
            return fail(Error::RenderStateFailed);

        GhosttyRenderStateDirty dirty{GHOSTTY_RENDER_STATE_DIRTY_FALSE};
        GhosttyRenderStateColors colors{GHOSTTY_INIT_SIZED(GhosttyRenderStateColors)};
        GhosttyRenderStateCursor cursor{GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor)};
        uint16_t cols{0};
        uint16_t height{0};
        const GhosttyRenderStateData keys[] = {GHOSTTY_RENDER_STATE_DATA_COLS,
                                               GHOSTTY_RENDER_STATE_DATA_ROWS,
                                               GHOSTTY_RENDER_STATE_DATA_DIRTY,
                                               GHOSTTY_RENDER_STATE_DATA_COLORS,
                                               GHOSTTY_RENDER_STATE_DATA_CURSOR};
        void *values[] = {&cols, &height, &dirty, &colors, &cursor};

        if (ghostty_render_state_get_multi(render, std::size(keys), keys, values, nullptr) != GHOSTTY_SUCCESS)
            return false;

        const bool reuseCells{!contentInvalidated && !feeding && frame.columns == cols && frame.rows.size() == height};
        const bool full = dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL || frame.columns != cols || frame.rows.size() != height
            || frame.selectionForeground != selectionForeground || frame.selectionBackground != selectionBackground;
        ++frame.generation;
        frame.columns = cols;
        frame.rows.resize(height);
        frame.rowWrapped.resize(height);
        frame.rowVersions.resize(height);
        frame.rowIdentities.resize(height);
        frame.background = QColor::fromRgb(rgb(colors.background));
        frame.foreground = QColor::fromRgb(rgb(colors.foreground));
        frame.cursorColor = QColor::fromRgb(rgb(colors.cursor_has_value ? colors.cursor : colors.foreground));
        frame.selectionForeground = selectionForeground;
        frame.selectionBackground = selectionBackground;
        frame.cursor = cursor.viewport_has_value ? QPoint(cursor.viewport_x, cursor.viewport_y) : QPoint(-1, -1);
        frame.cursorVisible = cursor.visible;
        frame.cursorBlinking = cursor.blinking;
        frame.cursorWideTail = cursor.wide_tail;
        frame.cursorStyle = cursorStyle(cursor.visual_style);
        GhosttyTerminalScrollbar scrollbar{};
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
        frame.scrollTotal = scrollbar.total;
        frame.scrollOffset = scrollbar.offset;
        auto rowsHandle = rowIterator.value;
        ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rowsHandle);
        int rowIndex{0};

        while (ghostty_render_state_row_iterator_next(rowIterator)) {
            bool rowDirty{false};
            ghostty_render_state_row_get(rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &rowDirty);
            const int y{rowIndex++};
            GhosttyRenderStateRowId rowIdentity{};
            ghostty_render_state_row_get(rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &rowIdentity);
            frame.rowIdentities[y] = {rowIdentity.bits[0], rowIdentity.bits[1]};

            if (!full && !rowDirty)
                continue;

            GhosttyRenderStateRowSelection selected{GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection)};
            const bool hasRowSelection{ghostty_render_state_row_get(rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selected) == GHOSTTY_SUCCESS};

            if (reuseCells) {
                // Avoid detaching shared rows whose selection bits have not changed.
                const auto &previous = frame.rows.at(y);
                qsizetype first{0};

                while (first < previous.size()
                       && bool(previous[first].flags & Selected) == (hasRowSelection && first >= selected.start_x && first <= selected.end_x))
                    ++first;

                if (first < previous.size()) {
                    auto cells = previous;

                    for (qsizetype x{first}; x < cells.size(); ++x) {
                        auto &cell = cells[x];

                        if (bool(cell.flags & Selected) != (hasRowSelection && x >= selected.start_x && x <= selected.end_x))
                            cell.flags ^= Selected;
                    }

                    frame.rows[y] = std::move(cells);
                    frame.rowVersions[y] = frame.generation;
                }

                continue;
            }

            QVector<Cell> cells;
            cells.reserve(cols);
            GhosttyRow rawRow{0};
            ghostty_render_state_row_get(rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &rawRow);
            bool wrap{false};
            ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_WRAP, &wrap);
            frame.rowWrapped[y] = wrap;
            auto cellsHandle = rowCells.value;
            ghostty_render_state_row_get(rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cellsHandle);

            while (ghostty_render_state_row_cells_next(rowCells)) {
                Cell cell;
                GhosttyCell raw{0};
                GhosttyStyle style{GHOSTTY_INIT_SIZED(GhosttyStyle)};
                const GhosttyRenderStateRowCellsData cellKeys[] = {GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE};
                void *cellValues[] = {&raw, &style};
                ghostty_render_state_row_cells_get_multi(rowCells, std::size(cellKeys), cellKeys, cellValues, nullptr);
                GhosttyCellWide wide{GHOSTTY_CELL_WIDE_NARROW};
                ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
                cell.width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : wide == GHOSTTY_CELL_WIDE_NARROW ? 1 : 0;
                std::array<uint8_t, 64> text{};
                GhosttyBuffer buffer{text.data(), text.size(), 0};
                auto result = ghostty_render_state_row_cells_get(rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer);

                if (result == GHOSTTY_OUT_OF_SPACE) {
                    QByteArray expanded(qsizetype(buffer.len), Qt::Uninitialized);
                    buffer.ptr = reinterpret_cast<uint8_t *>(expanded.data());
                    buffer.cap = size_t(expanded.size());

                    if (ghostty_render_state_row_cells_get(rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer) == GHOSTTY_SUCCESS)
                        cell.text = QString::fromUtf8(expanded);
                } else if (result == GHOSTTY_SUCCESS) {
                    const QUtf8StringView grapheme{reinterpret_cast<const char *>(text.data()), qsizetype(buffer.len)};
                    const auto &previous = frame.rows.at(y);
                    const qsizetype x{cells.size()};
                    cell.text = x < previous.size() && previous[x].text == grapheme ? previous[x].text : QString::fromUtf8(grapheme);
                }

                GhosttyColorRgb fg{colors.foreground};
                GhosttyColorRgb bg{colors.background};
                ghostty_render_state_row_cells_get(rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);

                if (style.bold && style.fg_color.tag == GHOSTTY_STYLE_COLOR_NONE && boldForeground.isValid()) {
                    fg = rgb(boldForeground);
                }

                const bool explicitBackground =
                    ghostty_render_state_row_cells_get(rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
                cell.flags = (style.bold ? Bold : 0) | (style.italic ? Italic : 0) | (style.faint ? Faint : 0) | (style.blink ? Blink : 0)
                    | (style.inverse ? Inverse : 0) | (style.invisible ? Invisible : 0) | (style.strikethrough ? Strike : 0) | (style.overline ? Overline : 0)
                    | (style.underline ? Underline : 0);

                if (cell.text.startsWith(QStringView(u"\U0010eeee")))
                    cell.flags |= ImagePlaceholder;

                const bool isSelected{hasRowSelection && cells.size() >= selected.start_x && cells.size() <= selected.end_x};

                if (isSelected)
                    cell.flags |= Selected;

                if (style.inverse)
                    std::swap(fg, bg);

                cell.foreground = rgb(fg);
                cell.background = rgb(bg);
                cell.defaultBackground = !explicitBackground && !style.inverse;
                cell.defaultForeground = style.fg_color.tag == GHOSTTY_STYLE_COLOR_NONE && !style.inverse;
                cell.underlineColor = style.underline_color.tag == GHOSTTY_STYLE_COLOR_RGB ? rgb(style.underline_color.value.rgb)
                    : style.underline_color.tag == GHOSTTY_STYLE_COLOR_PALETTE             ? rgb(colors.palette[style.underline_color.value.palette])
                                                                                           : cell.foreground;
                cell.underline = quint8(style.underline);
                cells.append(std::move(cell));
            }

            frame.rows[y] = std::move(cells);
            frame.rowVersions[y] = frame.generation;
        }

        quint64 placementsGeneration{0};
        ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION, &placementsGeneration);

        // The generation includes image pixels and animation frames as well as placements.
        if (frame.generation == 1 || frame.imagesGeneration != placementsGeneration) {
            frame.imagesGeneration = placementsGeneration;
            frame.images.clear();
            QSet<quint32> visibleImages;
            auto imagesHandle = imageIterator.value;
            ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &imagesHandle);

            while (ghostty_render_state_image_iterator_next(imageIterator)) {
                ImagePlacement placement;
                int32_t x{0};
                int32_t y{0};
                uint32_t offsetX{0};
                uint32_t offsetY{0};
                uint32_t width{0};
                uint32_t imageHeight{0};
                uint32_t imageColumns{0};
                uint32_t imageRows{0};
                uint32_t sourceX{0};
                uint32_t sourceY{0};
                uint32_t sourceWidth{0};
                uint32_t sourceHeight{0};
                GhosttyRenderStateImageLayer layer{GHOSTTY_RENDER_STATE_IMAGE_LAYER_ABOVE_TEXT};
                const GhosttyRenderStateImageData imageKeys[] = {
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_PLACEMENT_ID,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_Z,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_COLS,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_ROWS,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,
                    GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT,
                };

                void *imageValues[] = {&placement.imageId,
                                       &placement.generation,
                                       &placement.placementId,
                                       &layer,
                                       &placement.z,
                                       &x,
                                       &y,
                                       &offsetX,
                                       &offsetY,
                                       &width,
                                       &imageHeight,
                                       &imageColumns,
                                       &imageRows,
                                       &sourceX,
                                       &sourceY,
                                       &sourceWidth,
                                       &sourceHeight};

                if (ghostty_render_state_image_get_multi(imageIterator, std::size(imageKeys), imageKeys, imageValues, nullptr) != GHOSTTY_SUCCESS)
                    continue;

                switch (layer) {
                case GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_BACKGROUND:
                    placement.layer = ImageLayer::BelowBackground;
                    break;
                case GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_TEXT:
                    placement.layer = ImageLayer::BelowText;
                    break;
                case GHOSTTY_RENDER_STATE_IMAGE_LAYER_ABOVE_TEXT:
                    placement.layer = ImageLayer::AboveText;
                    break;
                default:
                    continue;
                }

                placement.cell = QPoint(x, y);
                placement.pixelOffset = QPoint(int(offsetX), int(offsetY));
                placement.pixelSize = QSize(int(width), int(imageHeight));
                placement.gridSize = QSize(int(imageColumns), int(imageRows));
                placement.source = QRect(int(sourceX), int(sourceY), int(sourceWidth), int(sourceHeight));
                placement.image = image(placement.imageId, placement.generation);

                if (placement.image.isNull())
                    continue;

                placement.generation = quint64(placement.image.cacheKey());
                visibleImages.insert(placement.imageId);
                frame.images.append(std::move(placement));
            }

            imageCache.removeIf([&visibleImages](auto it) {
                return !visibleImages.contains(it.key());
            });

            std::stable_sort(frame.images.begin(), frame.images.end(), [](const ImagePlacement &a, const ImagePlacement &b) {
                if (a.layer != b.layer)
                    return a.layer < b.layer;

                if (a.z != b.z)
                    return a.z < b.z;

                if (a.imageId != b.imageId)
                    return a.imageId < b.imageId;

                return a.placementId < b.placementId;
            });
        }

        ghostty_render_state_clean(render);

        // A synchronized-output callback may have captured a frame before the last bytes of this feed.
        contentInvalidated = feeding;

        return true;
    }

    Terminal *q;

    Handle<GhosttyTerminal, ghostty_terminal_free> terminal;
    Handle<GhosttyRenderState, ghostty_render_state_free> render;
    Handle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free> rowIterator;
    Handle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free> rowCells;
    Handle<GhosttyRenderStateImageIterator, ghostty_render_state_image_iterator_free> imageIterator;
    Handle<GhosttyKeyEncoder, ghostty_key_encoder_free> keyEncoder;
    Handle<GhosttyKeyEvent, ghostty_key_event_free> keyEvent;
    Handle<GhosttyMouseEncoder, ghostty_mouse_encoder_free> mouseEncoder;
    Handle<GhosttyMouseEvent, ghostty_mouse_event_free> mouseEvent;
    GhosttySelectionGesture gesture{nullptr};
    Error error{Error::None};
    QByteArray identity{"libghostty-qt"};
    int columns{80};
    int rows{24};
    QSize cellPixels{1, 1};
    Frame frame;
    QColor boldForeground;
    QColor selectionForeground;
    QColor selectionBackground;
    bool held{false};
    bool feeding{false};
    bool contentInvalidated{true};
    bool compressionEnabled{false};
    std::optional<uint64_t> compressionActivity;
    Progress progress;
    std::optional<Notification> notification;
    ClipboardWriteHandler clipboardWriteHandler;
    QTimer holdTimer;
    QTimer animationTimer;
    QTimer compressionTimer;
    QElapsedTimer clock;
    QHash<quint32, std::pair<quint64, QImage>> imageCache;
};

Terminal::Terminal(int columns, int rows, qint64 scrollbackLines, QObject *parent)
    : QObject{parent}
    , d{std::make_unique<Private>(this)}
{
    static std::atomic<quint64> nextTerminalId{1};
    d->frame.terminalId = nextTerminalId.fetch_add(1, std::memory_order_relaxed);
    static std::once_flag once;
    std::call_once(once, [] {
        ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, reinterpret_cast<const void *>(decodePng));
    });

    d->columns = std::clamp(columns, 1, 65535);
    d->rows = std::clamp(rows, 1, 65535);

    if (ghostty_terminal_new(nullptr, d->terminal.out(), quint16(d->columns), quint16(d->rows)) != GHOSTTY_SUCCESS
        || ghostty_render_state_new(nullptr, d->render.out()) != GHOSTTY_SUCCESS
        || ghostty_render_state_row_iterator_new(nullptr, d->rowIterator.out()) != GHOSTTY_SUCCESS
        || ghostty_render_state_row_cells_new(nullptr, d->rowCells.out()) != GHOSTTY_SUCCESS
        || ghostty_render_state_image_iterator_new(nullptr, d->imageIterator.out()) != GHOSTTY_SUCCESS
        || ghostty_key_encoder_new(nullptr, d->keyEncoder.out()) != GHOSTTY_SUCCESS || ghostty_key_event_new(nullptr, d->keyEvent.out()) != GHOSTTY_SUCCESS
        || ghostty_mouse_encoder_new(nullptr, d->mouseEncoder.out()) != GHOSTTY_SUCCESS
        || ghostty_mouse_event_new(nullptr, d->mouseEvent.out()) != GHOSTTY_SUCCESS || ghostty_selection_gesture_new(nullptr, &d->gesture) != GHOSTTY_SUCCESS) {
        d->error = Error::AllocationFailed;

        return;
    }

    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_USERDATA, d.get());
    auto setCallback = [this](GhosttyTerminalOption option, auto callback) {
        ghostty_terminal_set(d->terminal, option, reinterpret_cast<const void *>(+callback));
    };

    setCallback(GHOSTTY_TERMINAL_OPT_WRITE_PTY, [](GhosttyTerminal, void *data, const uint8_t *bytes, size_t length) {
        Q_EMIT static_cast<Private *>(data)->q->writeRequested(QByteArray(reinterpret_cast<const char *>(bytes), qsizetype(length)));
    });

    setCallback(GHOSTTY_TERMINAL_OPT_BELL, [](GhosttyTerminal, void *data) {
        Q_EMIT static_cast<Private *>(data)->q->bell();
    });

    setCallback(GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, [](GhosttyTerminal terminal, void *data) {
        GhosttyString title{};
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title);

        Q_EMIT static_cast<Private *>(data)->q->titleChanged(string(title));
    });

    setCallback(GHOSTTY_TERMINAL_OPT_PWD_CHANGED, [](GhosttyTerminal, void *data) {
        Q_EMIT static_cast<Private *>(data)->q->workingDirectoryChanged();
    });

    setCallback(GHOSTTY_TERMINAL_OPT_SIZE, [](GhosttyTerminal, void *data, GhosttySizeReportSize *size) -> bool {
        const auto *d = static_cast<Private *>(data);
        *size = {quint16(d->rows), quint16(d->columns), quint32(d->cellPixels.width()), quint32(d->cellPixels.height())};

        return true;
    });

    setCallback(GHOSTTY_TERMINAL_OPT_XTVERSION, [](GhosttyTerminal, void *data) -> GhosttyString {
        const auto &identity = static_cast<Private *>(data)->identity;

        return {reinterpret_cast<const uint8_t *>(identity.constData()), size_t(identity.size())};
    });

    setCallback(GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION, [](GhosttyTerminal, void *data, const GhosttyTerminalDesktopNotification *message) {
        auto *d = static_cast<Private *>(data);
        d->notification = Notification{notificationText(message->title, 256), notificationText(message->body, 1024)};
    });

    setCallback(GHOSTTY_TERMINAL_OPT_PROGRESS_REPORT, [](GhosttyTerminal, void *data, const GhosttyTerminalProgressReport *progress) {
        auto *d = static_cast<Private *>(data);

        switch (progress->state) {
        case GHOSTTY_TERMINAL_PROGRESS_STATE_SET:
            d->progress.state = Progress::State::Normal;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_ERROR:
            d->progress.state = Progress::State::Error;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_INDETERMINATE:
            d->progress.state = Progress::State::Indeterminate;
            break;
        case GHOSTTY_TERMINAL_PROGRESS_STATE_PAUSE:
            d->progress.state = Progress::State::Paused;
            break;
        default:
            d->progress.state = Progress::State::None;
            break;
        }

        // If a pause omits the percentage, keep the previous value.
        if (d->progress.state != Progress::State::Paused || progress->progress >= 0) {
            d->progress.percent = std::clamp(int(progress->progress), -1, 100);
        }
    });

    setCallback(GHOSTTY_TERMINAL_OPT_COLOR_SCHEME, [](GhosttyTerminal terminal, void *, GhosttyColorScheme *scheme) -> bool {
        GhosttyColorRgb background{};
        // An application's OSC background override must not mask the user's theme preference.
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND_DEFAULT, &background);
        *scheme = colorScheme(background);

        return true;
    });

    setCallback(GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES, [](GhosttyTerminal, void *data, GhosttyDeviceAttributes *attributes) -> bool {
        const auto *d = static_cast<Private *>(data);
        *attributes = {};
        attributes->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
        attributes->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
        attributes->primary.num_features = 1;

        if (d->clipboardWriteHandler) {
            attributes->primary.features[attributes->primary.num_features++] = GHOSTTY_DA_FEATURE_CLIPBOARD;
        }

        attributes->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
        attributes->secondary.firmware_version = 10;

        return true;
    });

    setCallback(GHOSTTY_TERMINAL_OPT_RESET, [](GhosttyTerminal, void *data) {
        auto *d = static_cast<Private *>(data);
        d->resetGesture();
        d->notification.reset();
        d->progress = {};

        Q_EMIT d->q->titleChanged({});
        Q_EMIT d->q->workingDirectoryChanged();
    });

    setCallback(GHOSTTY_TERMINAL_OPT_RENDER_HOLD, [](GhosttyTerminal, void *data, bool held) {
        auto *d = static_cast<Private *>(data);

        if (held) {
            // Capture before later bytes in the same feed can change the frame.
            d->capture();
            d->holdTimer.start(std::chrono::milliseconds{150});
        } else {
            d->holdTimer.stop();
        }

        d->held = held;
    });

    setCallback(GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, [](GhosttyTerminal, void *data, const GhosttyClipboardWrite *request) {
        const auto *d = static_cast<Private *>(data);
        GhosttyClipboardWriteReply reply{GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply)};
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;

        if (d->clipboardWriteHandler) {
            const auto mode = request->location == GHOSTTY_CLIPBOARD_LOCATION_STANDARD ? QClipboard::Clipboard : QClipboard::Selection;
            std::unique_ptr<QMimeData> contents;

            if (request->contents_len) {
                contents = std::make_unique<QMimeData>();

                for (size_t i{0}; i < request->contents_len; ++i) {
                    const auto &content = request->contents[i];
                    contents->setData(string(content.mime), QByteArray(reinterpret_cast<const char *>(content.data.ptr), qsizetype(content.data.len)));
                }
            }

            reply.result =
                d->clipboardWriteHandler(std::move(contents), mode) ? GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS : GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;
        }

        if (request->reply) {
            request->reply(request, &reply);
        }
    });

    bool denied{false};

    // Disable glyph queries because the C API cannot consult the renderer's fonts
    // and would report every system glyph missing.
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_GLYPH_PROTOCOL, &denied);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &denied);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &denied);
    const size_t imageLimit{ImageLimit};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &imageLimit);
    const size_t continuationLimit{1024 * 1024};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES, &continuationLimit);

    // Enable Ghostty's Unicode width default, which the C library does not set.
    const GhosttyTerminalModeConfig graphemeDefault{GHOSTTY_MODE_GRAPHEME_CLUSTER, true};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &graphemeDefault);
    setColors(Qt::white, Qt::black, {});
    setScrollbackLines(scrollbackLines);
    resize(d->columns, d->rows, {1, 1});
}

Terminal::~Terminal()
{
    d->assertThread();
}

void Terminal::setClipboardWriteHandler(ClipboardWriteHandler handler)
{
    d->assertThread();
    d->clipboardWriteHandler = std::move(handler);
}

bool Terminal::isValid() const
{
    return d->terminal && d->render && d->error == Error::None;
}

Terminal::Error Terminal::error() const
{
    return d->error;
}

bool Terminal::feed(QByteArrayView bytes)
{
    d->assertThread();

    if (!isValid() || d->feeding)
        return false;

    d->feeding = true;
    ghostty_terminal_vt_write(d->terminal, reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size()));
    d->feeding = false;
    bool processingError{false};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR, &processingError);
    d->tickImages();

    d->invalidateFrame();

    return !processingError;
}

bool Terminal::resize(int columns, int rows, QSize cellPixels)
{
    d->assertThread();

    if (!isValid())
        return false;

    columns = std::clamp(columns, 1, 65535);
    rows = std::clamp(rows, 1, 65535);

    if (!cellPixels.isEmpty())
        d->cellPixels = cellPixels;

    if (ghostty_terminal_resize(d->terminal, quint16(columns), quint16(rows), quint32(d->cellPixels.width()), quint32(d->cellPixels.height()))
        != GHOSTTY_SUCCESS)
        return false;

    const bool sizeChanged{d->columns != columns || d->rows != rows};
    d->columns = columns;
    d->rows = rows;

    if (sizeChanged) {
        Q_EMIT geometryChanged();
    }

    d->invalidateFrame();

    return true;
}

bool Terminal::snapshot(Frame &frame)
{
    d->assertThread();

    if (!isValid() || (!d->held && !d->capture()))
        return false;

    frame = d->frame;

    return true;
}

void Terminal::setScrollbackLines(qint64 lines)
{
    d->assertThread();
    Q_ASSERT(lines >= UnlimitedScrollback);
    const size_t limit{size_t(std::min(quint64(lines), quint64(SIZE_MAX)))};

    // Unlimited removes the line limit. Ghostty still bounds uncompressed page storage.
    const size_t byteLimit{lines == UnlimitedScrollback ? size_t{150'000'000} : 0};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, lines <= 0 ? &byteLimit : nullptr);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, lines > 0 ? &limit : nullptr);

    d->compressionEnabled = lines == UnlimitedScrollback;

    if (!d->compressionEnabled) {
        d->compressionTimer.stop();
        d->compressionActivity.reset();
    }

    d->invalidateFrame();
}

void Terminal::setColors(QColor foreground, QColor background, const QVector<QRgb> &palette, QColor boldForeground)
{
    d->assertThread();
    const auto fg = rgb(foreground), bg = rgb(background);
    GhosttyColorRgb previousForeground{}, previousBackground{}, colors[256]{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND_DEFAULT, &previousForeground);
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND_DEFAULT, &previousBackground);
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, colors);
    bool changed{rgb(previousForeground) != rgb(fg) || rgb(previousBackground) != rgb(bg) || d->boldForeground != boldForeground};

    for (qsizetype i{0}; i < std::min<qsizetype>(palette.size(), 256); ++i) {
        const auto color = rgb(QColor::fromRgb(palette[i]));
        changed = changed || rgb(colors[i]) != rgb(color);
        colors[i] = color;
    }

    if (!changed)
        return;

    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, colors);
    d->boldForeground = boldForeground;

    // Ghostty does not track boldForeground. Mark the frame dirty when it changes.
    const GhosttyRenderStateDirty dirty{GHOSTTY_RENDER_STATE_DIRTY_FULL};
    ghostty_render_state_set(d->render, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &dirty);
    d->invalidateFrame();

    if (d->mode(GHOSTTY_MODE_COLOR_SCHEME_REPORT)) {
        char bytes[16];
        size_t length{0};

        if (ghostty_color_scheme_report_encode(colorScheme(bg), bytes, sizeof(bytes), &length) == GHOSTTY_SUCCESS)
            Q_EMIT writeRequested(QByteArray(bytes, qsizetype(length)));
    }
}

void Terminal::setDecorations(QColor cursor, QColor selectionForeground, QColor selectionBackground)
{
    d->assertThread();
    auto color = rgb(cursor);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, cursor.isValid() ? &color : nullptr);
    d->selectionForeground = selectionForeground;
    d->selectionBackground = selectionBackground;

    d->invalidateFrame();
}

void Terminal::setDefaultCursor(CursorStyle style, bool blinking)
{
    d->assertThread();
    const auto value = cursorStyle(style);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE, &value);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blinking);

    d->invalidateFrame();
}

void Terminal::setTerminfoName(QByteArrayView name)
{
    d->assertThread();
    const GhosttyString value{reinterpret_cast<const uint8_t *>(name.data()), size_t(name.size())};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &value);
}

void Terminal::setApplicationIdentity(QByteArrayView identity)
{
    d->assertThread();
    d->identity.clear();

    for (char c : identity) {
        if (c >= 0x20 && c < 0x7f)
            d->identity.append(c);

        if (d->identity.size() == 256)
            break;
    }
}

void Terminal::scroll(qint64 rows)
{
    d->assertThread();
    GhosttyTerminalScrollViewport scroll{GHOSTTY_SCROLL_VIEWPORT_DELTA, {}};
    scroll.value.delta = intptr_t(std::clamp<qint64>(rows, INTPTR_MIN, INTPTR_MAX));
    ghostty_terminal_scroll_viewport(d->terminal, scroll);

    d->invalidateFrame();
}

void Terminal::scrollTo(quint64 offset)
{
    d->assertThread();
    GhosttyTerminalScrollViewport scrollRequest{GHOSTTY_SCROLL_VIEWPORT_ROW, {}};
    scrollRequest.value.row = size_t(std::min(offset, quint64(SIZE_MAX)));
    ghostty_terminal_scroll_viewport(d->terminal, scrollRequest);

    d->invalidateFrame();
}

void Terminal::scrollBottom()
{
    d->assertThread();
    ghostty_terminal_scroll_viewport(d->terminal, {GHOSTTY_SCROLL_VIEWPORT_BOTTOM, {}});

    d->invalidateFrame();
}

void Terminal::scrollPrompt(int delta)
{
    d->assertThread();
    GhosttyTerminalScrollViewport scrollRequest{GHOSTTY_SCROLL_VIEWPORT_DELTA_PROMPT, {}};
    scrollRequest.value.delta_prompt = delta;
    ghostty_terminal_scroll_viewport(d->terminal, scrollRequest);

    d->invalidateFrame();
}

bool Terminal::select(QPoint first, QPoint last, bool rectangle)
{
    d->assertThread();
    d->resetGesture();
    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};
    selection.start = d->ref(first);
    selection.end = d->ref(last);
    selection.rectangle = rectangle;

    return selection.start.node && selection.end.node && d->setSelection(&selection);
}

bool Terminal::selectionGesture(const SelectionEvent &input)
{
    d->assertThread();
    GhosttySelectionGestureEventType type{GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS};

    switch (input.type) {
    case SelectionEvent::Type::Press:
        type = GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS;
        break;
    case SelectionEvent::Type::Release:
        type = GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE;
        break;
    case SelectionEvent::Type::Drag:
        type = GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG;
        break;
    case SelectionEvent::Type::Autoscroll:
        type = GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK;
        break;
    }

    Handle<GhosttySelectionGestureEvent, ghostty_selection_gesture_event_free> event;

    if (ghostty_selection_gesture_event_new(nullptr, event.out(), type) != GHOSTTY_SUCCESS)
        return false;

    const GhosttySurfacePosition position{input.position.x(), input.position.y()};

    if (input.type != SelectionEvent::Type::Autoscroll) {
        const auto ref = d->ref(input.cell);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, ref.node ? &ref : nullptr);
    }

    if (input.type == SelectionEvent::Type::Press) {
        const uint64_t time{input.timestamp * 1000000};
        const uint64_t interval{quint64(input.repeatInterval) * 1000000};
        const double distance{double(input.repeatDistance)};
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &time);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS, &interval);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &distance);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);

        if (input.rectangle) {
            const GhosttySelectionGestureBehaviors behavior{GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL,
                                                            GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL,
                                                            GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL};
            ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_BEHAVIORS, &behavior);
        }
    } else if (input.type != SelectionEvent::Type::Release) {
        const GhosttySelectionGestureGeometry geometry{quint32(d->columns), quint32(d->cellPixels.width()), 0, quint32(d->rows * d->cellPixels.height())};
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &input.rectangle);

        if (input.type == SelectionEvent::Type::Autoscroll) {
            const GhosttyPointCoordinate viewport{quint16(input.cell.x()), quint32(input.cell.y())};
            ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &viewport);
        }
    }

    GhosttyTerminalScrollbar before{};

    if (input.type == SelectionEvent::Type::Autoscroll)
        ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &before);

    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};
    const auto result = ghostty_selection_gesture_event(d->gesture, d->terminal, event, &selection);

    if (result != GHOSTTY_SUCCESS && result != GHOSTTY_NO_VALUE)
        return false;

    if (input.type == SelectionEvent::Type::Release)
        return true;

    if (result == GHOSTTY_NO_VALUE && input.type == SelectionEvent::Type::Drag) {
        GhosttySelectionGestureBehavior behavior{GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL};
        ghostty_selection_gesture_get(d->gesture, d->terminal, GHOSTTY_SELECTION_GESTURE_DATA_BEHAVIOR, &behavior);

        // A word or line drag without a new range should retain the selection.
        if (behavior != GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL)
            return true;
    }

    bool viewportChanged{false};

    if (input.type == SelectionEvent::Type::Autoscroll) {
        GhosttyTerminalScrollbar after{};
        ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &after);
        viewportChanged = before.offset != after.offset;

        if (result == GHOSTTY_NO_VALUE) {
            if (viewportChanged)
                d->invalidateFrame();

            return true;
        }
    }

    return d->setSelection(result == GHOSTTY_SUCCESS ? &selection : nullptr, viewportChanged);
}

bool Terminal::selectionAutoscroll() const
{
    d->assertThread();
    GhosttySelectionGestureAutoscroll value{GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE};
    ghostty_selection_gesture_get(d->gesture, d->terminal, GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL, &value);

    return value != GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
}

void Terminal::cancelSelectionGesture()
{
    d->assertThread();
    d->resetGesture();
}

std::optional<QPoint> Terminal::adjustSelection(SelectionAdjustment adjustment, bool extendSelection)
{
    d->assertThread();
    GhosttySelectionAdjust nativeAdjustment;

    switch (adjustment) {
    case SelectionAdjustment::Left:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_LEFT;
        break;
    case SelectionAdjustment::Right:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_RIGHT;
        break;
    case SelectionAdjustment::Up:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_UP;
        break;
    case SelectionAdjustment::Down:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_DOWN;
        break;
    case SelectionAdjustment::Start:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_HOME;
        break;
    case SelectionAdjustment::End:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_END;
        break;
    case SelectionAdjustment::PageUp:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_PAGE_UP;
        break;
    case SelectionAdjustment::PageDown:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_PAGE_DOWN;
        break;
    case SelectionAdjustment::StartOfLine:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_BEGINNING_OF_LINE;
        break;
    case SelectionAdjustment::EndOfLine:
        nativeAdjustment = GHOSTTY_SELECTION_ADJUST_END_OF_LINE;
        break;
    default:
        std::unreachable();
    }

    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};

    if (ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &selection) != GHOSTTY_SUCCESS
        || ghostty_terminal_selection_adjust(d->terminal, &selection, nativeAdjustment) != GHOSTTY_SUCCESS)
        return std::nullopt;

    if (!extendSelection)
        selection.start = selection.end;

    GhosttyPointCoordinate end{};

    if (ghostty_terminal_point_from_grid_ref(d->terminal, &selection.end, GHOSTTY_POINT_TAG_SCREEN, &end) != GHOSTTY_SUCCESS)
        return std::nullopt;

    const bool selectionChanged{!d->selectionEquals(&selection)};

    if (selectionChanged && ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection) != GHOSTTY_SUCCESS)
        return std::nullopt;

    GhosttyTerminalScrollbar scrollbar{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
    const quint64 offset{std::clamp(quint64(scrollbar.offset), end.y >= quint32(d->rows) ? quint64(end.y - d->rows + 1) : 0, quint64(end.y))};
    const bool viewportChanged{offset != scrollbar.offset};

    if (viewportChanged) {
        GhosttyTerminalScrollViewport scrollRequest{};
        scrollRequest.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
        scrollRequest.value.row = offset;
        ghostty_terminal_scroll_viewport(d->terminal, scrollRequest);
        ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
    }

    // Preserve the raw native endpoint for vertical movement. Its displayed
    // cursor skips wide-character and wrapping spacers in the new viewport.
    QPoint cursor{int(end.x), int(end.y - scrollbar.offset)};

    while (cursor.x() > 0) {
        const auto ref = d->ref(cursor);
        GhosttyCell cell{};
        GhosttyCellWide wide{GHOSTTY_CELL_WIDE_NARROW};

        if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS || ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide) != GHOSTTY_SUCCESS
            || (wide != GHOSTTY_CELL_WIDE_SPACER_TAIL && wide != GHOSTTY_CELL_WIDE_SPACER_HEAD))
            break;

        --cursor.rx();
    }

    if (viewportChanged)
        d->invalidateFrame();
    else if (selectionChanged)
        Q_EMIT frameChanged();

    return cursor;
}

bool Terminal::hasSelection() const
{
    d->assertThread();
    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};

    return ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &selection) == GHOSTTY_SUCCESS;
}

void Terminal::clearSelection()
{
    d->assertThread();
    d->resetGesture();
    d->setSelection(nullptr);
}

bool Terminal::selectAll()
{
    d->assertThread();
    d->resetGesture();
    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};

    return ghostty_terminal_select_all(d->terminal, &selection) == GHOSTTY_SUCCESS && d->setSelection(&selection);
}

std::unique_ptr<Search> Terminal::createSearch()
{
    d->assertThread();
    auto search = std::unique_ptr<Search>{new Search};
    search->d->terminal = this;
    ghostty_search_new(nullptr, &search->d->handle, d->terminal);

    return search;
}

bool Terminal::selectSearchMatch(const Search &search, bool forward)
{
    d->assertThread();
    GhosttyTerminalScrollbar before{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &before);

    if (ghostty_search_set(search.d->handle, forward ? GHOSTTY_SEARCH_OPT_SELECT_NEXT : GHOSTTY_SEARCH_OPT_SELECT_PREV, nullptr) != GHOSTTY_SUCCESS)
        return false;

    GhosttyTerminalScrollbar after{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &after);

    if (before.offset != after.offset)
        d->invalidateFrame();

    return true;
}

bool Terminal::applySearchSelection(const Search &search)
{
    d->assertThread();
    GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};
    const auto result = ghostty_search_get(search.d->handle, GHOSTTY_SEARCH_DATA_SELECTED_MATCH, &selection);

    if (result != GHOSTTY_SUCCESS && result != GHOSTTY_NO_VALUE) {
        return false;
    }

    d->resetGesture();

    return d->setSelection(result == GHOSTTY_SUCCESS ? &selection : nullptr);
}

std::optional<QRegion> Terminal::searchViewportMatches(const Search &search) const
{
    d->assertThread();

    // A synchronized-output frame can precede the live search data. Never
    // paint current match positions over that frozen terminal content.
    if (d->held)
        return QRegion{};

    GhosttySelectionBuffer buffer{};
    const auto result = ghostty_search_get(search.d->handle, GHOSTTY_SEARCH_DATA_VIEWPORT_MATCHES, &buffer);

    if (result != GHOSTTY_SUCCESS && result != GHOSTTY_OUT_OF_SPACE)
        return std::nullopt;

    QVector<GhosttySelection> matches(qsizetype(buffer.len));
    buffer.ptr = matches.data();
    buffer.cap = size_t(matches.size());

    if (ghostty_search_get(search.d->handle, GHOSTTY_SEARCH_DATA_VIEWPORT_MATCHES, &buffer) != GHOSTTY_SUCCESS)
        return std::nullopt;

    GhosttyTerminalScrollbar viewport{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &viewport);
    const auto position = [&](const GhosttyGridRef &ref) -> std::optional<QPoint> {
        GhosttyPointCoordinate point{};

        if (ghostty_terminal_point_from_grid_ref(d->terminal, &ref, GHOSTTY_POINT_TAG_VIEWPORT, &point) == GHOSTTY_SUCCESS)
            return QPoint(point.x, int(point.y));

        // A wrapped match can cross a viewport edge. Resolve only these
        // offscreen endpoints in screen coordinates, then clip its rows.
        if (ghostty_terminal_point_from_grid_ref(d->terminal, &ref, GHOSTTY_POINT_TAG_SCREEN, &point) != GHOSTTY_SUCCESS)
            return std::nullopt;

        return QPoint(point.x, int(std::clamp(qint64(point.y) - qint64(viewport.offset), qint64(-1), qint64(d->rows))));
    };
    QRegion region;

    for (const auto &match : std::as_const(matches)) {
        const auto start = position(match.start);
        auto end = position(match.end);

        if (!start || !end)
            continue;

        GhosttyCell cell{};
        GhosttyCellWide wide{GHOSTTY_CELL_WIDE_NARROW};

        if (ghostty_grid_ref_cell(&match.end, &cell) == GHOSTTY_SUCCESS && ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide) == GHOSTTY_SUCCESS
            && wide == GHOSTTY_CELL_WIDE_WIDE)
            end->rx() += 1;

        for (int y{qMax(0, start->y())}; y <= qMin(d->rows - 1, end->y()); ++y) {
            const int first{y == start->y() ? start->x() : 0};
            const int last{y == end->y() ? qMin(d->columns - 1, end->x()) : d->columns - 1};
            region += QRect(first, y, last - first + 1, 1);
        }
    }

    return region;
}

QString Terminal::selectionText() const
{
    d->assertThread();

    return formattedSelection(d->terminal, nullptr);
}

QString Terminal::historyText() const
{
    d->assertThread();
    GhosttyFormatterTerminalOptions options{GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions)};
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    Handle<GhosttyFormatter, ghostty_formatter_free> formatter;

    if (ghostty_formatter_terminal_new(nullptr, formatter.out(), d->terminal, options) != GHOSTTY_SUCCESS)
        return {};

    uint8_t *bytes{nullptr};
    size_t length{0};

    if (ghostty_formatter_format_alloc(formatter, nullptr, &bytes, &length) != GHOSTTY_SUCCESS)
        return {};

    const QString result{QString::fromUtf8(reinterpret_cast<const char *>(bytes), qsizetype(length))};
    ghostty_free(nullptr, bytes, length);

    return result;
}

std::unique_ptr<HistoryReader> Terminal::createHistoryReader()
{
    d->assertThread();
    auto reader = std::unique_ptr<HistoryReader>{new HistoryReader};
    reader->d->owner = this;
    reader->d->terminal = d->terminal;

    if (!reader->d->initialize()) {
        return {};
    }

    return reader;
}

QString Terminal::commandOutput() const
{
    d->assertThread();
    GhosttySelection search{GHOSTTY_INIT_SIZED(GhosttySelection)};
    search.end = GHOSTTY_INIT_SIZED(GhosttyGridRef);

    if (ghostty_terminal_grid_ref(d->terminal, point({d->columns - 1, d->rows - 1}, GHOSTTY_POINT_TAG_ACTIVE), &search.end) != GHOSTTY_SUCCESS)
        return {};

    search.start = search.end;

    // Find the latest output cell before calling select_output to avoid repeated
    // history scans when the shell provides no semantic prompt markers.
    for (;;) {
        GhosttyCell candidate{0};
        bool hasText{false};
        GhosttyCellSemanticContent semantic{GHOSTTY_CELL_SEMANTIC_OUTPUT};
        const GhosttyCellData keys[] = {GHOSTTY_CELL_DATA_HAS_TEXT, GHOSTTY_CELL_DATA_SEMANTIC_CONTENT};
        void *values[] = {&hasText, &semantic};

        if (ghostty_grid_ref_cell(&search.end, &candidate) != GHOSTTY_SUCCESS
            || ghostty_cell_get_multi(candidate, std::size(keys), keys, values, nullptr) != GHOSTTY_SUCCESS)
            return {};

        if (hasText && semantic == GHOSTTY_CELL_SEMANTIC_OUTPUT) {
            GhosttySelection selection{GHOSTTY_INIT_SIZED(GhosttySelection)};

            return ghostty_terminal_select_output(d->terminal, search.end, &selection) == GHOSTTY_SUCCESS ? formattedSelection(d->terminal, &selection)
                                                                                                          : QString();
        }

        const auto previous = search.end;

        if (ghostty_terminal_selection_adjust(d->terminal, &search, GHOSTTY_SELECTION_ADJUST_LEFT) != GHOSTTY_SUCCESS
            || (search.end.node == previous.node && search.end.x == previous.x && search.end.y == previous.y))
            return {};
    }
}

QString Terminal::linkAt(QPoint cell) const
{
    d->assertThread();
    const auto ref = d->ref(cell);
    size_t size{0};

    if (!ref.node || ghostty_grid_ref_hyperlink_uri(&ref, nullptr, 0, &size) != GHOSTTY_OUT_OF_SPACE)
        return {};

    QByteArray text(qsizetype(size), Qt::Uninitialized);

    if (ghostty_grid_ref_hyperlink_uri(&ref, reinterpret_cast<uint8_t *>(text.data()), size, &size) != GHOSTTY_SUCCESS)
        return {};

    return QString::fromUtf8(text);
}

QString Terminal::workingDirectoryUrl() const
{
    d->assertThread();
    GhosttyString value{};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_PWD, &value);

    return string(value);
}

void Terminal::clearScrollback()
{
    d->assertThread();
    clearSelection();
    size_t previous{0};
    const auto result = ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES, &previous);
    const size_t zero{0};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &zero);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, result == GHOSTTY_NO_VALUE ? nullptr : &previous);

    d->invalidateFrame();
}

void Terminal::clearProcessMetadata()
{
    d->assertThread();
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_PWD, nullptr);
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_TITLE, nullptr);
    d->progress = {};
    d->notification.reset();
}

void Terminal::reset()
{
    d->assertThread();
    clearSelection();
    ghostty_terminal_reset(d->terminal);
    d->progress = {};
    d->notification.reset();

    Q_EMIT titleChanged({});
    Q_EMIT workingDirectoryChanged();

    d->invalidateFrame();
}

Progress Terminal::progress() const
{
    d->assertThread();

    return d->progress;
}

std::optional<Notification> Terminal::takeNotification()
{
    d->assertThread();

    return std::exchange(d->notification, std::nullopt);
}

QByteArray Terminal::encodeKey(const QKeyEvent &event, KeyboardLocks locks)
{
    d->assertThread();

    if (event.type() != QEvent::KeyPress && event.type() != QEvent::KeyRelease)
        return {};

    const auto input = keyInput(event);
    ghostty_key_encoder_setopt_from_terminal(d->keyEncoder, d->terminal);
    ghostty_key_event_set_action(d->keyEvent, input.action);
    ghostty_key_event_set_key(d->keyEvent, input.key);
    ghostty_key_event_set_mods(d->keyEvent, input.modifiers | (locks.capsLock ? GHOSTTY_MODS_CAPS_LOCK : 0) | (locks.numLock ? GHOSTTY_MODS_NUM_LOCK : 0));
    ghostty_key_event_set_consumed_mods(d->keyEvent, input.consumedModifiers);
    ghostty_key_event_set_unshifted_codepoint(d->keyEvent, input.codepoint);
    ghostty_key_event_set_utf8(d->keyEvent, input.text.constData(), size_t(input.text.size()));
    QByteArray result(128, Qt::Uninitialized);
    size_t length{0};
    auto status = ghostty_key_encoder_encode(d->keyEncoder, d->keyEvent, result.data(), size_t(result.size()), &length);

    if (status == GHOSTTY_OUT_OF_SPACE) {
        result.resize(qsizetype(length));
        status = ghostty_key_encoder_encode(d->keyEncoder, d->keyEvent, result.data(), length, &length);
    }

    if (status != GHOSTTY_SUCCESS)
        return {};

    result.resize(qsizetype(length));

    return result;
}

QByteArray Terminal::encodeMouse(const MouseEvent &event)
{
    d->assertThread();
    ghostty_mouse_encoder_setopt_from_terminal(d->mouseEncoder, d->terminal);
    GhosttyMouseEncoderSize size{GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize)};
    size.screen_width = quint32(std::max(1.0, event.size.width()));
    size.screen_height = quint32(std::max(1.0, event.size.height()));
    size.cell_width = quint32(std::max(1.0, event.cellSize.width()));
    size.cell_height = quint32(std::max(1.0, event.cellSize.height()));
    ghostty_mouse_encoder_setopt(d->mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    const bool pressed{event.buttons != Qt::NoButton};
    ghostty_mouse_encoder_setopt(d->mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &pressed);
    ghostty_mouse_event_set_action(d->mouseEvent,
                                   event.action == MouseEvent::Action::Press         ? GHOSTTY_MOUSE_ACTION_PRESS
                                       : event.action == MouseEvent::Action::Release ? GHOSTTY_MOUSE_ACTION_RELEASE
                                                                                     : GHOSTTY_MOUSE_ACTION_MOTION);
    GhosttyMouseButton button{GHOSTTY_MOUSE_BUTTON_UNKNOWN};

    switch (event.wheel) {
    case MouseEvent::WheelDirection::Up:
        button = GHOSTTY_MOUSE_BUTTON_FOUR;
        break;
    case MouseEvent::WheelDirection::Down:
        button = GHOSTTY_MOUSE_BUTTON_FIVE;
        break;
    case MouseEvent::WheelDirection::Left:
        button = GHOSTTY_MOUSE_BUTTON_SIX;
        break;
    case MouseEvent::WheelDirection::Right:
        button = GHOSTTY_MOUSE_BUTTON_SEVEN;
        break;
    case MouseEvent::WheelDirection::None:
        switch (event.button) {
        case Qt::LeftButton:
            button = GHOSTTY_MOUSE_BUTTON_LEFT;
            break;
        case Qt::MiddleButton:
            button = GHOSTTY_MOUSE_BUTTON_MIDDLE;
            break;
        case Qt::RightButton:
            button = GHOSTTY_MOUSE_BUTTON_RIGHT;
            break;
        case Qt::BackButton:
            button = GHOSTTY_MOUSE_BUTTON_EIGHT;
            break;
        case Qt::ForwardButton:
            button = GHOSTTY_MOUSE_BUTTON_NINE;
            break;
        case Qt::ExtraButton3:
            button = GHOSTTY_MOUSE_BUTTON_TEN;
            break;
        case Qt::ExtraButton4:
            button = GHOSTTY_MOUSE_BUTTON_ELEVEN;
            break;
        default:
            break;
        }
    }

    if (button != GHOSTTY_MOUSE_BUTTON_UNKNOWN)
        ghostty_mouse_event_set_button(d->mouseEvent, button);
    else
        ghostty_mouse_event_clear_button(d->mouseEvent);
    ghostty_mouse_event_set_mods(d->mouseEvent, ghosttyModifiers(event.modifiers));
    ghostty_mouse_event_set_position(d->mouseEvent, {float(event.position.x()), float(event.position.y())});
    QByteArray result(128, Qt::Uninitialized);
    size_t length{0};

    if (ghostty_mouse_encoder_encode(d->mouseEncoder, d->mouseEvent, result.data(), size_t(result.size()), &length) != GHOSTTY_SUCCESS)
        return {};

    result.resize(qsizetype(length));

    return result;
}

QByteArray Terminal::encodeFocus(bool focused) const
{
    d->assertThread();

    if (!isModeEnabled(Mode::FocusEvents))
        return {};

    char bytes[8];
    size_t length{0};

    if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, bytes, sizeof(bytes), &length) != GHOSTTY_SUCCESS)
        return {};

    return QByteArray(bytes, qsizetype(length));
}

QByteArray Terminal::encodePaste(QByteArrayView bytes) const
{
    d->assertThread();
    QByteArray data(bytes);
    QByteArray result(data.size() + 32, Qt::Uninitialized);
    size_t length{0};

    if (ghostty_paste_encode(data.data(), size_t(data.size()), isModeEnabled(Mode::BracketedPaste), result.data(), size_t(result.size()), &length)
        != GHOSTTY_SUCCESS)
        return {};

    result.resize(qsizetype(length));

    return result;
}

bool Terminal::isModeEnabled(Mode mode) const
{
    d->assertThread();

    switch (mode) {
    case Mode::BracketedPaste:
        return d->mode(GHOSTTY_MODE_BRACKETED_PASTE);
    case Mode::FocusEvents:
        return d->mode(GHOSTTY_MODE_FOCUS_EVENT);
    case Mode::AlternateScroll:
        return d->mode(GHOSTTY_MODE_ALT_SCROLL);
    case Mode::SynchronizedOutput:
        return d->mode(GHOSTTY_MODE_SYNC_OUTPUT);
    case Mode::AlternateScreen: {
        GhosttyTerminalScreen screen{GHOSTTY_TERMINAL_SCREEN_PRIMARY};
        ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);

        return screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    }
    }

    return false;
}

Qt::CursorShape Terminal::mouseCursorShape() const
{
    d->assertThread();
    GhosttyMouseShape shape{GHOSTTY_MOUSE_SHAPE_TEXT};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE, &shape);

    switch (shape) {
    case GHOSTTY_MOUSE_SHAPE_HELP:
        return Qt::WhatsThisCursor;
    case GHOSTTY_MOUSE_SHAPE_POINTER:
        return Qt::PointingHandCursor;
    case GHOSTTY_MOUSE_SHAPE_PROGRESS:
        return Qt::BusyCursor;
    case GHOSTTY_MOUSE_SHAPE_WAIT:
        return Qt::WaitCursor;
    case GHOSTTY_MOUSE_SHAPE_CELL:
    case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:
        return Qt::CrossCursor;
    case GHOSTTY_MOUSE_SHAPE_TEXT:
    case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT:
        return Qt::IBeamCursor;
    case GHOSTTY_MOUSE_SHAPE_ALIAS:
        return Qt::DragLinkCursor;
    case GHOSTTY_MOUSE_SHAPE_COPY:
        return Qt::DragCopyCursor;
    case GHOSTTY_MOUSE_SHAPE_MOVE:
    case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL:
        return Qt::SizeAllCursor;
    case GHOSTTY_MOUSE_SHAPE_NO_DROP:
    case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:
        return Qt::ForbiddenCursor;
    case GHOSTTY_MOUSE_SHAPE_GRAB:
        return Qt::OpenHandCursor;
    case GHOSTTY_MOUSE_SHAPE_GRABBING:
        return Qt::ClosedHandCursor;
    case GHOSTTY_MOUSE_SHAPE_COL_RESIZE:
        return Qt::SplitHCursor;
    case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE:
        return Qt::SplitVCursor;
    case GHOSTTY_MOUSE_SHAPE_N_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_S_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:
        return Qt::SizeVerCursor;
    case GHOSTTY_MOUSE_SHAPE_E_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_W_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:
        return Qt::SizeHorCursor;
    case GHOSTTY_MOUSE_SHAPE_NE_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_SW_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE:
        return Qt::SizeBDiagCursor;
    case GHOSTTY_MOUSE_SHAPE_NW_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_SE_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE:
        return Qt::SizeFDiagCursor;
    default:
        return Qt::ArrowCursor;
    }
}

bool Terminal::mouseTracking() const
{
    d->assertThread();
    bool enabled{false};
    ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &enabled);

    return enabled;
}

void Terminal::endSynchronizedOutput()
{
    d->assertThread();
    GhosttyTerminalModeConfig config{GHOSTTY_MODE_SYNC_OUTPUT, false};
    ghostty_terminal_set(d->terminal, GHOSTTY_TERMINAL_OPT_MODE, &config);
    d->held = false;
    d->holdTimer.stop();

    d->invalidateFrame();
}

QString Terminal::version()
{
    GhosttyString value{};
    ghostty_build_info(GHOSTTY_BUILD_INFO_VERSION_STRING, &value);

    return string(value);
}
}
