// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "TerminalNode.h"
#include "FontMetrics.h"
#include "GlyphAtlas_p.h"
#include "GridShaper_p.h"
#include "PreeditLayout.h"
#include "Symbols_p.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMatrix4x4>
#include <QPair>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGMaterialShader>
#include <QSGRectangleNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QVector>
#include <QtClassHelperMacros>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

static void initializeTerminalShaders()
{
    Q_INIT_RESOURCE(terminal_shaders);
}

namespace GhosttyQt::Quick
{
namespace
{
struct Vertex {
    float x, y, u, v;
    float r, g, b, a;
};

const QSGGeometry::AttributeSet &attributes()
{
    static const QSGGeometry::Attribute values[]{QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
                                                 QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
                                                 QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType)};
    static const QSGGeometry::AttributeSet result{3, sizeof(Vertex), values};

    return result;
}

// Must match terminal.frag.
enum class AtlasPass {
    Texture = 0,
    Red = 1,
    Green = 2,
    Blue = 3,
    Subpixel = 4,
};

class AtlasMaterial;
class AtlasShader final : public QSGMaterialShader
{
public:
    AtlasShader()
    {
        setFlag(UpdatesGraphicsPipelineState);
        setShaderFileName(VertexStage, QStringLiteral(":/ghostty-qt-quick/shaders/terminal.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/ghostty-qt-quick/shaders/terminal.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *material, QSGMaterial *oldMaterial) override;
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture, QSGMaterial *material, QSGMaterial *) override;
    bool updateGraphicsPipelineState(RenderState &state, GraphicsPipelineState *pipeline, QSGMaterial *material, QSGMaterial *) override;
};

class AtlasMaterial final : public QSGMaterial
{
public:
    explicit AtlasMaterial(std::shared_ptr<AtlasPage> value,
                           const QRectF &bounds,
                           GlyphImage::Format glyphFormat,
                           QColor color,
                           bool aligned,
                           AtlasPass blendPass)
        : page{std::move(value)}
        , sampleBounds{bounds}
        , format{glyphFormat}
        , foreground{color}
        , pixelAligned{aligned}
        , pass{blendPass}
    {
        setFlag(Blending);
        setFlag(RequiresFullMatrixExceptTranslate);
    }

    QSGMaterialType *type() const override
    {
        static QSGMaterialType type;

        return &type;
    }

    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override
    {
        return new AtlasShader;
    }

    int compare(const QSGMaterial *other) const override
    {
        const auto *rhs = static_cast<const AtlasMaterial *>(other);

        if (page.get() != rhs->page.get()) {
            return std::less<const AtlasPage *>()(page.get(), rhs->page.get()) ? -1 : 1;
        }

        const auto left =
            std::make_tuple(sampleBounds.x(), sampleBounds.y(), sampleBounds.width(), sampleBounds.height(), format, foreground.rgba(), pixelAligned, pass);
        const auto right = std::make_tuple(rhs->sampleBounds.x(),
                                           rhs->sampleBounds.y(),
                                           rhs->sampleBounds.width(),
                                           rhs->sampleBounds.height(),
                                           rhs->format,
                                           rhs->foreground.rgba(),
                                           rhs->pixelAligned,
                                           rhs->pass);

        return left == right ? 0 : left < right ? -1 : 1;
    }

    std::shared_ptr<AtlasPage> page;
    QRectF sampleBounds;
    GlyphImage::Format format;
    QColor foreground;
    bool pixelAligned;
    AtlasPass pass;
};

bool isTranslation(const QMatrix4x4 &matrix)
{
    return matrix(0, 0) == 1 && matrix(1, 1) == 1 && matrix(0, 1) == 0 && matrix(1, 0) == 0 && matrix(3, 0) == 0 && matrix(3, 1) == 0 && matrix(3, 2) == 0
        && matrix(3, 3) == 1;
}

// Passing RenderState by value adds copies in the shader callbacks.
// clazy:exclude-next-line=function-args-by-value
bool useSubpixel(const QSGMaterialShader::RenderState &state, const AtlasMaterial &material)
{
    if (material.format != GlyphImage::Format::Subpixel || state.opacity() != 1) {
        return false;
    }

    return isTranslation(state.modelViewMatrix());
}

bool AtlasShader::updateUniformData(RenderState &state, QSGMaterial *material, QSGMaterial *oldMaterial)
{
    bool changed{false};
    auto *buffer = state.uniformData();

    if (state.isMatrixDirty()) {
        std::memcpy(buffer->data(), state.modelViewMatrix().constData(), 64);
        std::memcpy(buffer->data() + 64, state.projectionMatrix().constData(), 64);
        changed = true;
    }

    const auto *current = static_cast<AtlasMaterial *>(material);

    if (!oldMaterial || current->sampleBounds != static_cast<AtlasMaterial *>(oldMaterial)->sampleBounds) {
        const auto &bounds = current->sampleBounds;
        const float values[]{float(bounds.left()), float(bounds.top()), float(bounds.right()), float(bounds.bottom())};
        std::memcpy(buffer->data() + 128, values, sizeof(values));
        changed = true;
    }

    const float mode{current->format == GlyphImage::Format::Subpixel ? float(current->pass) * (useSubpixel(state, *current) ? 1.0f : -1.0f) : 0.0f};
    const float parameters[]{state.opacity(), state.devicePixelRatio(), current->pixelAligned && isTranslation(state.modelViewMatrix()) ? 1.0f : 0.0f, mode};

    // Compare the bytes being uploaded, including their floating-point encodings.
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    if (std::memcmp(buffer->constData() + 144, parameters, sizeof(parameters))) {
        std::memcpy(buffer->data() + 144, parameters, sizeof(parameters));
        changed = true;
    }

    return changed;
}

void AtlasShader::updateSampledImage(RenderState &state, int binding, QSGTexture **texture, QSGMaterial *material, QSGMaterial *)
{
    if (binding == 1) {
        *texture = static_cast<AtlasMaterial *>(material)->page->texture.get();

        if (*texture) {
            (*texture)->setFiltering(static_cast<AtlasMaterial *>(material)->pixelAligned && isTranslation(state.modelViewMatrix()) ? QSGTexture::Nearest
                                                                                                                                    : QSGTexture::Linear);
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
    }
}

bool AtlasShader::updateGraphicsPipelineState(RenderState &state, GraphicsPipelineState *pipeline, QSGMaterial *material, QSGMaterial *)
{
    const auto *current = static_cast<AtlasMaterial *>(material);
    pipeline->blendEnable = true;
    pipeline->separateBlendFactors = true;
    pipeline->srcColor = GraphicsPipelineState::One;
    pipeline->dstColor = GraphicsPipelineState::OneMinusSrcAlpha;
    pipeline->srcAlpha = GraphicsPipelineState::One;
    pipeline->dstAlpha = GraphicsPipelineState::OneMinusSrcAlpha;
    pipeline->colorWrite = GraphicsPipelineState::R | GraphicsPipelineState::G | GraphicsPipelineState::B | GraphicsPipelineState::A;
    const bool subpixel{useSubpixel(state, *current)};

    if (current->pass == AtlasPass::Subpixel && subpixel) {
        pipeline->srcColor = GraphicsPipelineState::ConstantColor;
        pipeline->dstColor = GraphicsPipelineState::OneMinusSrcColor;
        pipeline->blendConstant = current->foreground;
    } else if (current->pass != AtlasPass::Texture && current->pass != AtlasPass::Subpixel && subpixel) {
        // Blend one coverage channel per pass to preserve glyph overlap order.
        // Leave destination alpha unchanged for opaque LCD text.
        pipeline->colorWrite = GraphicsPipelineState::ColorMask(1 << (int(current->pass) - 1));
    } else if (current->pass == AtlasPass::Green || current->pass == AtlasPass::Blue) {
        // The first pass alone renders the complete grayscale fallback.
        pipeline->colorWrite = {};
    }

    return true;
}

void clearChildren(QSGNode *node)
{
    while (auto *child = node->firstChild()) {
        node->removeChildNode(child);
        delete child;
    }
}

void setClipRect(QSGClipNode *node, const QRectF &rect)
{
    if (node->geometry() && node->clipRect() == rect) {
        return;
    }

    node->setIsRectangular(true);
    node->setClipRect(rect);

    if (!node->geometry()) {
        // Qt still needs geometry for stencil clipping when the rectangle is transformed.
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
    }

    QSGGeometry::updateRectGeometry(node->geometry(), rect);
    node->markDirty(QSGNode::DirtyGeometry);
}

struct ColorQuad {
    QRectF rect;
    QColor color;
};

void colorGeometry(QSGNode *parent, const QVector<ColorQuad> &quads, TerminalNode::Statistics *statistics)
{
    if (quads.isEmpty()) {
        return;
    }

    auto *node = new QSGGeometryNode;
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), quads.size() * 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
    auto *vertices = geometry->vertexDataAsColoredPoint2D();

    for (const auto &quad : quads) {
        const QRectF &r = quad.rect;
        const QColor &c = quad.color;
        const auto red = uchar(qRound(c.redF() * c.alphaF() * 255));
        const auto green = uchar(qRound(c.greenF() * c.alphaF() * 255));
        const auto blue = uchar(qRound(c.blueF() * c.alphaF() * 255));
        const auto alpha = uchar(c.alpha());
        const QPointF points[]{r.topLeft(), r.bottomLeft(), r.topRight(), r.topRight(), r.bottomLeft(), r.bottomRight()};

        for (const auto &point : points) {
            (vertices++)->set(point.x(), point.y(), red, green, blue, alpha);
        }
    }

    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    parent->appendChildNode(node);

    if (statistics) {
        statistics->geometryBytes += quads.size() * 6 * sizeof(QSGGeometry::ColoredPoint2D);
    }
}

class GlyphBufferPool
{
public:
    [[nodiscard]] QVector<Vertex> take()
    {
        if (m_count == 0) {
            return {};
        }

        auto vertices = std::move(m_buffers[--m_count]);
        m_capacity -= vertices.capacity();

        return vertices;
    }

    void recycle(QVector<Vertex> vertices)
    {
        const qsizetype capacity{vertices.capacity()};

        if (capacity == 0 || m_count == m_buffers.size() || capacity > MaximumCapacity - m_capacity) {
            return;
        }

        vertices.clear();
        m_capacity += capacity;
        m_buffers[m_count++] = std::move(vertices);
    }

private:
    static constexpr qsizetype MaximumCapacity{256 * 1024 / sizeof(Vertex)};
    std::array<QVector<Vertex>, 64> m_buffers;
    size_t m_count{0};
    qsizetype m_capacity{0};
};

class GlyphGeometry
{
public:
    explicit GlyphGeometry(GlyphBufferPool &buffers)
        : m_buffers{buffers}
    {
    }

    ~GlyphGeometry()
    {
        for (auto &batch : m_batches) {
            m_buffers.recycle(std::move(batch.vertices));
        }
    }

    void
    append(const GlyphImage &image, const QRectF &rect, QRectF source, QColor color, const QRectF &sampleBounds = QRectF(0, 0, 1, 1), bool pixelAligned = true)
    {
        if (!image.page || rect.isEmpty()) {
            return;
        }

        const QColor foreground{image.format == GlyphImage::Format::Subpixel ? color : QColor()};
        qsizetype index{qsizetype(m_batches.size()) - 1};

        for (; index >= 0; --index) {
            if (m_batches[index].page == image.page && m_batches[index].sampleBounds == sampleBounds && m_batches[index].format == image.format
                && m_batches[index].pixelAligned == pixelAligned) {
                break;
            }

            if (m_batches[index].bounds.intersects(rect)) {
                index = -1;
                break;
            }
        }

        if (index < 0) {
            index = m_batches.size();
            m_batches.push_back({image.page, sampleBounds, rect, m_buffers.take(), image.format, foreground, pixelAligned, false});
        } else {
            auto &batch = m_batches[index];
            batch.bounds = batch.bounds.united(rect);

            if (batch.format == GlyphImage::Format::Subpixel && !batch.channelPasses && batch.foreground != foreground) {
                batch.channelPasses = true;
                batch.foreground = {};
            }
        }

        auto &vertices = m_batches[index].vertices;
        const QSizeF size{image.page->image.size()};
        source.translate(image.pixels.topLeft());
        const QRectF uv(source.x() / size.width(), source.y() / size.height(), source.width() / size.width(), source.height() / size.height());
        const float alpha{color.alphaF()};
        const float red{color.redF() * alpha};
        const float green{color.greenF() * alpha};
        const float blue{color.blueF() * alpha};
        const QPointF points[]{rect.topLeft(), rect.bottomLeft(), rect.topRight(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()};
        const QPointF coords[]{uv.topLeft(), uv.bottomLeft(), uv.topRight(), uv.topRight(), uv.bottomLeft(), uv.bottomRight()};
        const qsizetype offset{vertices.size()};
        vertices.resizeForOverwrite(offset + 6);
        auto *quad = vertices.data() + offset;

        for (int i{0}; i < 6; ++i) {
            quad[i] = Vertex{float(points[i].x()), float(points[i].y()), float(coords[i].x()), float(coords[i].y()), red, green, blue, alpha};
        }
    }

    void finish(QSGNode *parent, TerminalNode::Statistics *statistics)
    {
        auto emitBatch = [&](const Batch &batch, AtlasPass pass) {
            auto *node = new QSGGeometryNode;
            auto *geometry = new QSGGeometry(attributes(), batch.vertices.size());
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->setVertexDataPattern(QSGGeometry::StaticPattern);
            const auto bytes = batch.vertices.size() * sizeof(Vertex);
            std::memcpy(geometry->vertexData(), batch.vertices.constData(), bytes);
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setMaterial(new AtlasMaterial(batch.page,
                                                batch.sampleBounds,
                                                batch.format,
                                                pass == AtlasPass::Subpixel ? batch.foreground : QColor(),
                                                batch.pixelAligned,
                                                pass));
            node->setFlag(QSGNode::OwnsMaterial);
            parent->appendChildNode(node);

            if (statistics) {
                statistics->geometryBytes += bytes;
            }
        };

        for (const auto &batch : m_batches) {
            if (batch.channelPasses) {
                for (const auto pass : {AtlasPass::Red, AtlasPass::Green, AtlasPass::Blue}) {
                    emitBatch(batch, pass);
                }
            } else {
                emitBatch(batch, batch.format == GlyphImage::Format::Subpixel ? AtlasPass::Subpixel : AtlasPass::Texture);
            }
        }
    }

private:
    struct Batch {
        std::shared_ptr<AtlasPage> page;
        QRectF sampleBounds;
        QRectF bounds;
        QVector<Vertex> vertices;
        GlyphImage::Format format{GlyphImage::Format::Grayscale};
        QColor foreground;
        bool pixelAligned{false};
        bool channelPasses{false};
    };

    GlyphBufferPool &m_buffers;
    std::vector<Batch> m_batches;
};

char32_t singleCodepoint(const QString &text)
{
    if (text.size() == 1) {
        return text[0].unicode();
    }

    if (text.size() == 2 && text[0].isHighSurrogate() && text[1].isLowSurrogate()) {
        return QChar::surrogateToUcs4(text[0], text[1]);
    }

    return 0;
}

bool isCursorCell(int column, int cursor, const QVector<Cell> &cells)
{
    return cursor >= 0 && column >= cursor && column < cursor + qMax(1, int(cells[cursor].width));
}
}

class TerminalNode::Private
{
    Q_DISABLE_COPY_MOVE(Private)

public:
    explicit Private(TerminalNode *node, QQuickWindow *window, bool collectStatistics)
        : window{window}
        , statistics{collectStatistics ? std::optional{Statistics{}} : std::nullopt}
        , atlas{std::make_unique<GlyphAtlas>(window, statistics ? &statistics->rasterizedGlyphs : nullptr)}
    {
        for (auto *&padding : background) {
            padding = window->createRectangleNode();
            node->appendChildNode(padding);
        }

        clip = new QSGClipNode;
        clip->setIsRectangular(true);
        node->appendChildNode(clip);

        for (auto *layer : {&defaultBackground, &belowBackground, &cellBackground, &belowText, &cellText, &aboveText, &cursor, &preedit}) {
            *layer = new QSGNode;
            clip->appendChildNode(*layer);
        }
    }

    struct Row {
        QSGTransformNode *base{nullptr};
        QSGTransformNode *background{nullptr};
        QSGTransformNode *foreground{nullptr};
        QVector<Cell> cells;
        QRegion searchMatches;
        QRegion hoveredLink;
        QList<TextHighlight> textHighlights; // Row-local cell coordinates.
        std::array<quint64, 2> identity{};
        quint64 version{0};
        int cursorColumn{-1};
        bool blink{false};
    };

    struct ImageTexture {
        quint32 id{0};
        quint64 generation{0};
        std::shared_ptr<AtlasPage> page;
    };

    QQuickWindow *window;
    std::array<QSGRectangleNode *, 4> background;
    QSGClipNode *clip;
    QSGNode *defaultBackground{nullptr};
    QSGNode *belowBackground{nullptr};
    QSGNode *cellBackground{nullptr};
    QSGNode *belowText{nullptr};
    QSGNode *cellText{nullptr};
    QSGNode *aboveText{nullptr};
    QSGNode *cursor{nullptr};
    QSGNode *preedit{nullptr};
    QVector<Row> rows;
    std::vector<ImageTexture> imageTextures;
    std::optional<Statistics> statistics;
    std::unique_ptr<GlyphAtlas> atlas;
    GlyphBufferPool glyphBuffers;
    GridShaper shaper;
    Frame frame;
    Options options;
    Frame pendingFrame;
    Options pendingOptions;
    bool pending{false};
    FontMetrics metrics;
    qsizetype compactionThreshold{GlyphAtlas::PixelBudget};
    bool initialized{false};

    QColor foreground(const Cell &cell, int column, int rowCursorColumn, const QVector<Cell> &cells, bool colored) const
    {
        QColor color{QColor::fromRgb(cell.foreground)};

        if (colored) {
            color = Qt::white;
        } else if (isCursorCell(column, rowCursorColumn, cells) && options.focused && frame.cursorStyle == CursorStyle::Block) {
            color = frame.background;
        } else if (cell.flags & Selected) {
            color = frame.selectionForeground.isValid() ? frame.selectionForeground : frame.background;
        }

        if (cell.flags & Faint) {
            color.setAlphaF(color.alphaF() * 0.5);
        }

        return color;
    }

    int cursorColumn(int row) const
    {
        if (!options.preedit.isEmpty() || frame.cursor.y() != row || !frame.cursorVisible
            || (frame.cursorBlinking && options.focused && !options.cursorBlinkVisible) || frame.cursor.x() < 0 || frame.cursor.x() >= frame.rows[row].size()) {
            return -1;
        }

        int column{frame.cursor.x()};

        if (column > 0 && (frame.cursorWideTail || frame.rows[row][column].width == 0)) {
            --column;
        }

        return column;
    }

    void appendGlyph(GlyphGeometry &geometry,
                     const GlyphImage &image,
                     const QRectF &rect,
                     int firstColumn,
                     int columnCount,
                     const QVector<Cell> &cells,
                     int rowCursorColumn,
                     bool pixelAligned = true)
    {
        if (!image.page || rect.isEmpty()) {
            return;
        }

        const qreal cw{metrics.cellSize.width()};
        const int lastColumn{qMin(int(cells.size()) - 1, firstColumn + columnCount - 1)};
        qreal left{rect.left()};

        while (left < rect.right() - 0.0001) {
            const int column{qBound(firstColumn, int(std::floor(left / cw)), lastColumn)};
            const qreal right{column >= lastColumn ? rect.right() : qMin(rect.right(), (column + 1) * cw)};

            if (right <= left) {
                break;
            }

            const Cell &cell = cells[column];

            if (!(cell.flags & Invisible) && (!(cell.flags & Blink) || options.textBlinkVisible)) {
                const QRectF part(left, rect.y(), right - left, rect.height());
                const QRectF source((part.x() - rect.x()) * image.pixels.width() / rect.width(),
                                    0,
                                    part.width() * image.pixels.width() / rect.width(),
                                    image.pixels.height());
                geometry.append(image,
                                part,
                                source,
                                foreground(cell, column, rowCursorColumn, cells, image.format == GlyphImage::Format::Color),
                                QRectF(0, 0, 1, 1),
                                pixelAligned);
            }

            left = right;
        }
    }

    void decorate(QVector<ColorQuad> &quads,
                  QVector<ColorQuad> &strikes,
                  GlyphGeometry &sprites,
                  const Cell &cell,
                  int column,
                  int rowCursorColumn,
                  const QVector<Cell> &cells)
    {
        if ((cell.flags & Invisible) || ((cell.flags & Blink) && !options.textBlinkVisible) || !cell.width
            || (!(cell.flags & (Strike | Overline | Underline)) && !cell.underline)) {
            return;
        }

        const qreal dpr{options.devicePixelRatio};
        const qreal x{column * metrics.cellSize.width()};
        const qreal width{cell.width * metrics.cellSize.width()};
        const qreal stroke{metrics.strokeWidth};
        const QColor fg{foreground(cell, column, rowCursorColumn, cells, false)};

        if (cell.flags & Strike) {
            strikes += {QRectF(x, metrics.strikePosition, width, stroke), fg};
        }

        if (cell.flags & Overline) {
            quads += {QRectF(x, 0, width, stroke), fg};
        }

        if (!cell.underline && !(cell.flags & Underline)) {
            return;
        }

        QColor color{QColor::fromRgb(cell.underlineColor)};

        if (cell.flags & (Selected | Faint) || isCursorCell(column, rowCursorColumn, cells)) {
            color = fg;
        }

        const qreal y{metrics.underlinePosition};
        // Allow decorations a quarter-cell overhang below the row.
        const qreal bottom{metrics.cellSize.height() + std::floor(metrics.cellSize.height() * dpr / 4) / dpr};
        const int style{cell.underline ? cell.underline : 1};

        if (style == 1) {
            quads += {QRectF(x, y, width, stroke), color};
        } else if (style == 2) {
            const qreal center{qMax(qreal(0), qMin(y, bottom - 2 * stroke))};
            quads += {QRectF(x, qMax(qreal(0), center - stroke), width, stroke), color};
            quads += {QRectF(x, center + stroke, width, stroke), color};
        } else if (style == 3 || style == 4) {
            const QSize size{qRound(metrics.cellSize.width() * dpr), qRound(metrics.cellSize.height() * dpr)};
            const auto image = atlas->underline(style, size, qRound(stroke * dpr));
            const QSizeF logicalSize{QSizeF(image.pixels.size()) / dpr};
            const qreal top{qBound(qreal(0), y + image.bearing.y() / dpr, bottom - logicalSize.height())};

            for (int offset{0}; offset < cell.width; ++offset) {
                sprites.append(image, QRectF(QPointF(x + offset * metrics.cellSize.width(), top), logicalSize), QRectF(QPointF(), image.pixels.size()), color);
            }
        } else {
            const int dots{style == DashDotUnderline ? 1 : style == DashDotDotUnderline ? 2 : 0};
            const qreal on{3 * stroke};
            const qreal period{on + 2 * stroke + dots * 3 * stroke};
            qreal start{std::floor(x / period) * period};

            // Fractional steps are needed for stroke-scaled patterns. The period is always positive.
            // NOLINTNEXTLINE(clang-analyzer-security.FloatLoopCounter)
            for (; start < x + width; start += period) {
                for (int part{0}; part <= dots; ++part) {
                    const qreal segment{start + (part ? on + 2 * stroke + (part - 1) * 3 * stroke : 0)};
                    const qreal left{qMax(segment, x)};
                    const qreal right{qMin(segment + (part ? stroke : on), x + width)};

                    if (right > left) {
                        quads += {QRectF(left, y, right - left, stroke), color};
                    }
                }
            }
        }
    }

    void rebuildRow(Row &row, const QVector<Cell> &sourceCells, int rowCursorColumn)
    {
        auto highlightedCells = sourceCells;

        for (const QRect &link : row.hoveredLink) {
            for (int column{link.left()}; column <= link.right(); ++column) {
                auto &cell = highlightedCells[column];
                cell.underline = cell.underline == 1 || (!cell.underline && (cell.flags & Underline)) ? 2 : 1;
            }
        }

        for (const auto &highlight : std::as_const(row.textHighlights)) {
            for (int column{highlight.cells.left()}; column <= highlight.cells.right() && column < highlightedCells.size(); ++column) {
                auto &cell = highlightedCells[column];
                // Keep an explicit underline color; otherwise it follows the text.
                if (cell.underlineColor == cell.foreground) {
                    cell.underlineColor = highlight.foreground;
                }

                cell.foreground = highlight.foreground;

                if (highlight.bold) {
                    cell.flags |= Bold;
                }

                if (highlight.faint) {
                    cell.flags |= Faint;
                }
            }
        }

        for (const QRect &match : row.searchMatches) {
            for (int column{match.left()}; column <= match.right(); ++column) {
                auto &cell = highlightedCells[column];
                cell.foreground = options.searchForeground.rgb();
                cell.background = options.searchBackground.rgb();
                cell.underlineColor = options.searchForeground.rgb();
                cell.defaultBackground = false;
            }
        }

        const auto &cells = std::as_const(highlightedCells);

        if (row.base) {
            clearChildren(row.base);
        }

        clearChildren(row.background);
        clearChildren(row.foreground);
        QVector<ColorQuad> backgrounds;
        QVector<ColorQuad> defaults;
        QVector<ColorQuad> decorations;
        QVector<ColorQuad> strikes;
        GlyphGeometry decorationSprites{glyphBuffers};
        GlyphGeometry glyphs{glyphBuffers};
        const qreal cw{metrics.cellSize.width()};
        const qreal ch{metrics.cellSize.height()};
        row.blink = false;

        for (int column{0}; column < cells.size(); ++column) {
            const Cell &cell = cells[column];
            row.blink |= bool(cell.flags & Blink);
            QColor color;

            if (isCursorCell(column, rowCursorColumn, cells) && options.focused && frame.cursorStyle == CursorStyle::Block) {
                color = frame.cursorColor;
            } else if (cell.flags & Selected) {
                color = frame.selectionBackground.isValid() ? frame.selectionBackground : frame.foreground;
            } else if (!cell.defaultBackground || (cell.flags & Inverse)) {
                color = QColor::fromRgb(cell.background);
            }

            if (color.isValid() || row.base) {
                const bool isDefault{!color.isValid()};

                if (isDefault) {
                    color = frame.background;
                    color.setAlphaF(qBound(qreal(0), options.backgroundOpacity, qreal(1)));
                }

                auto &quads = isDefault ? defaults : backgrounds;
                const QRectF rect(column * cw, 0, cw, ch);

                if (!quads.isEmpty() && quads.last().color == color && qFuzzyCompare(quads.last().rect.right(), rect.left())) {
                    quads.last().rect.setRight(rect.right());
                } else {
                    quads += {rect, color};
                }
            }

            decorate(decorations, strikes, decorationSprites, cell, column, rowCursorColumn, cells);
        }

        colorGeometry(row.background, backgrounds, statistics ? &*statistics : nullptr);

        if (row.base) {
            colorGeometry(row.base, defaults, statistics ? &*statistics : nullptr);
        }

        for (int begin{0}; begin < cells.size();) {
            const Cell &cell = cells[begin];

            if (!cell.width || cell.text.isEmpty() || (cell.flags & ImagePlaceholder)) {
                ++begin;
                continue;
            }

            const char32_t cp{singleCodepoint(cell.text)};

            if (isProceduralSymbol(cp)) {
                const QSize size(qRound(cw * options.devicePixelRatio * cell.width), qRound(ch * options.devicePixelRatio));
                const auto sprite = atlas->symbol(cp, size, qRound(metrics.strokeWidth * options.devicePixelRatio));
                appendGlyph(glyphs, sprite, QRectF(begin * cw, 0, cell.width * cw, ch), begin, cell.width, cells, rowCursorColumn);
                begin += cell.width;
                continue;
            }

            int end{begin + cell.width};

            const bool icon{isFontIcon(cell.text)};

            while (!icon && end < cells.size()) {
                const Cell &next = cells[end];

                // Fit icons independently of text ligatures and neighboring icons.
                if (!next.width || next.text.isEmpty() || (next.flags & ImagePlaceholder) || (next.flags & (Bold | Italic)) != (cell.flags & (Bold | Italic))
                    || isFontIcon(next.text) || isProceduralSymbol(singleCodepoint(next.text))) {
                    break;
                }

                end += next.width;
            }

            end = qMin(end, int(cells.size()));
            bool missed{false};
            const auto run = shaper.shape(cells, begin, end, statistics ? &missed : nullptr);

            if (statistics) {
                statistics->shapedRuns += missed;
            }

            for (const auto &glyph : run) {
                if (glyph.font.boundingRect(glyph.index).isEmpty()) {
                    continue;
                }

                const bool pixelAligned{glyph.scale == 1};
                const bool subpixel{pixelAligned && options.backgroundOpacity == 1 && frame.background.alpha() == 255};
                const auto image = atlas->glyph(glyph.font, glyph.index, subpixel);
                const qreal scale{glyph.scale / options.devicePixelRatio};
                const QPointF topLeft{QPointF(begin * cw, 0) + glyph.baseline / options.devicePixelRatio + image.bearing * scale};
                appendGlyph(glyphs,
                            image,
                            QRectF(topLeft, QSizeF(image.pixels.size()) * scale),
                            begin + glyph.firstColumn,
                            glyph.columnCount,
                            cells,
                            rowCursorColumn,
                            pixelAligned);
            }

            begin = end;
        }

        // Underlines z-sort below, strikethrough sorts above.
        colorGeometry(row.foreground, decorations, statistics ? &*statistics : nullptr);
        decorationSprites.finish(row.foreground, statistics ? &*statistics : nullptr);
        glyphs.finish(row.foreground, statistics ? &*statistics : nullptr);
        colorGeometry(row.foreground, strikes, statistics ? &*statistics : nullptr);
        row.cells = sourceCells;
        row.cursorColumn = rowCursorColumn;

        if (statistics) {
            ++statistics->rebuiltRows;
        }
    }

    void rebuildImages()
    {
        clearChildren(belowBackground);
        clearChildren(belowText);
        clearChildren(aboveText);
        std::vector<ImageTexture> next;

        for (const auto &placement : std::as_const(frame.images)) {
            if (placement.image.isNull()) {
                continue;
            }

            auto cached = std::find_if(next.begin(), next.end(), [&placement](const auto &entry) {
                return entry.id == placement.imageId && entry.generation == placement.generation;
            });

            if (cached == next.end()) {
                auto old = std::find_if(imageTextures.begin(), imageTextures.end(), [&placement](const auto &entry) {
                    return entry.id == placement.imageId && entry.generation == placement.generation;
                });

                if (old != imageTextures.end()) {
                    next.push_back(std::move(*old));
                } else {
                    auto *texture = window->createTextureFromImage(placement.image, QQuickWindow::TextureHasAlphaChannel);

                    if (!texture) {
                        continue;
                    }

                    auto page = std::make_shared<AtlasPage>();
                    page->image = placement.image;
                    page->texture.reset(texture);
                    page->texture->setFiltering(QSGTexture::Linear);
                    next.push_back({placement.imageId, placement.generation, std::move(page)});

                    if (statistics) {
                        statistics->textureUploadBytes += placement.image.sizeInBytes();
                    }
                }

                cached = std::prev(next.end());
            }

            const qreal dpr{options.devicePixelRatio};
            const QPointF position = options.contentRect.topLeft()
                + QPointF(placement.cell.x() * metrics.cellSize.width(), placement.cell.y() * metrics.cellSize.height()) + QPointF(placement.pixelOffset) / dpr;
            const QSizeF size = placement.pixelSize.isEmpty()
                ? QSizeF(placement.gridSize.width() * metrics.cellSize.width(), placement.gridSize.height() * metrics.cellSize.height())
                : QSizeF(placement.pixelSize) / dpr;
            QSGNode *layer = placement.layer == ImageLayer::BelowBackground ? belowBackground
                : placement.layer == ImageLayer::BelowText                  ? belowText
                                                                            : aboveText;
            const QRectF source{placement.source.isEmpty() ? placement.image.rect() : placement.source};

            // Clamp to the crop's texel centers to prevent neighboring pixels bleeding into it.
            const QSizeF pixels{placement.image.size()};
            const QRectF bounds((source.left() + 0.5) / pixels.width(),
                                (source.top() + 0.5) / pixels.height(),
                                qMax(qreal(0), source.width() - 1) / pixels.width(),
                                qMax(qreal(0), source.height() - 1) / pixels.height());
            GlyphGeometry geometry{glyphBuffers};
            geometry.append({cached->page, placement.image.rect(), {}, GlyphImage::Format::Color}, QRectF(position, size), source, Qt::white, bounds, false);
            geometry.finish(layer, statistics ? &*statistics : nullptr);
        }

        imageTextures = std::move(next);
    }

    void rebuildCursor()
    {
        clearChildren(cursor);

        if (frame.cursor.y() < 0 || frame.cursor.y() >= rows.size()) {
            return;
        }

        const int column{cursorColumn(frame.cursor.y())};

        if (column < 0) {
            return;
        }

        const qreal cw{metrics.cellSize.width()};
        const qreal ch{metrics.cellSize.height()};
        const qreal stroke{metrics.strokeWidth};
        const QPointF position{options.contentRect.topLeft() + QPointF(column * cw, frame.cursor.y() * ch)};
        const qreal width{qMax(1, int(frame.rows.at(frame.cursor.y()).at(column).width)) * cw};
        QVector<ColorQuad> quads;
        const QColor color{frame.cursorColor};

        if (!options.focused || frame.cursorStyle == CursorStyle::HollowBlock) {
            quads += {QRectF(position, QSizeF(width, stroke)), color};
            quads += {QRectF(position + QPointF(0, ch - stroke), QSizeF(width, stroke)), color};
            quads += {QRectF(position + QPointF(0, stroke), QSizeF(stroke, ch - 2 * stroke)), color};
            quads += {QRectF(position + QPointF(width - stroke, stroke), QSizeF(stroke, ch - 2 * stroke)), color};
        } else if (frame.cursorStyle == CursorStyle::Beam) {
            quads += {QRectF(position, QSizeF(stroke, ch)), color};
        } else if (frame.cursorStyle == CursorStyle::Underline) {
            quads += {QRectF(position + QPointF(0, ch - stroke), QSizeF(width, stroke)), color};
        } else if (frame.cursorStyle == CursorStyle::Block) {
            // Clip the shaped row to the cursor cell to preserve ligatures crossing its edges.
            auto *cursorClip = new QSGClipNode;
            setClipRect(cursorClip, QRectF(position, QSizeF(width, ch)));
            cursor->appendChildNode(cursorClip);
            Row overlay;
            overlay.background = new QSGTransformNode;
            overlay.foreground = new QSGTransformNode;
            QMatrix4x4 matrix;
            matrix.translate(options.contentRect.x(), position.y());
            overlay.background->setMatrix(matrix);
            overlay.foreground->setMatrix(matrix);
            cursorClip->appendChildNode(overlay.background);
            cursorClip->appendChildNode(overlay.foreground);
            rebuildRow(overlay, frame.rows[frame.cursor.y()], column);
        }

        colorGeometry(cursor, quads, statistics ? &*statistics : nullptr);
    }

    void rebuildPreedit()
    {
        clearChildren(preedit);

        if (options.preedit.isEmpty()) {
            return;
        }

        const auto layout = layoutPreedit(frame, options.preedit, options.preeditAttributes);

        for (const auto &line : layout.rows) {
            Row overlay;
            overlay.background = new QSGTransformNode;
            overlay.foreground = new QSGTransformNode;
            QMatrix4x4 matrix;
            matrix.translate(options.contentRect.x() + line.cell.x() * metrics.cellSize.width(),
                             options.contentRect.y() + line.cell.y() * metrics.cellSize.height());
            overlay.background->setMatrix(matrix);
            overlay.foreground->setMatrix(matrix);
            preedit->appendChildNode(overlay.background);
            preedit->appendChildNode(overlay.foreground);
            rebuildRow(overlay, line.cells, -1);
        }

        if (layout.caretVisible) {
            const QRectF caret(options.contentRect.topLeft()
                                   + QPointF(layout.caret.x() * metrics.cellSize.width(), layout.caret.y() * metrics.cellSize.height()),
                               QSizeF(metrics.strokeWidth, metrics.cellSize.height()));
            colorGeometry(preedit, {{caret, layout.caretColor}}, statistics ? &*statistics : nullptr);
        }
    }
};

TerminalNode::TerminalNode(QQuickWindow *window, bool collectStatistics)
    : d{std::make_unique<Private>(this, window, collectStatistics)}
{
    initializeTerminalShaders();
    setFlag(UsePreprocess);
}

TerminalNode::~TerminalNode()
{
    // Destroy geometry and materials before their atlas/image texture owners.
    clearChildren(this);
}

void TerminalNode::synchronize(const Frame &frame, const Options &options)
{
    // Defer shaping and rasterization to preprocess() so they do not block the GUI thread.
    d->pendingFrame = frame;
    d->pendingOptions = options;
    d->pending = true;
}

void TerminalNode::preprocess()
{
    if (std::exchange(d->pending, false)) {
        prepare(d->pendingFrame, d->pendingOptions);
    }
}

void TerminalNode::prepare(const Frame &frame, const Options &options)
{
    QElapsedTimer timer;

    if (d->statistics) {
        timer.start();
    }

    const bool fontChanged = !d->initialized || options.font != d->options.font || options.devicePixelRatio != d->options.devicePixelRatio
        || options.logicalDpi != d->options.logicalDpi;
    const bool terminalChanged{frame.terminalId != d->frame.terminalId};
    const bool appearanceChanged = fontChanged || terminalChanged || frame.foreground != d->frame.foreground || frame.background != d->frame.background
        || frame.cursorColor != d->frame.cursorColor || frame.selectionForeground != d->frame.selectionForeground
        || frame.selectionBackground != d->frame.selectionBackground || options.backgroundOpacity != d->options.backgroundOpacity
        || options.searchForeground != d->options.searchForeground || options.searchBackground != d->options.searchBackground;
    const bool cursorChanged = frame.cursor != d->frame.cursor || frame.cursorStyle != d->frame.cursorStyle || frame.cursorVisible != d->frame.cursorVisible
        || frame.cursorBlinking != d->frame.cursorBlinking || frame.cursorWideTail != d->frame.cursorWideTail || options.focused != d->options.focused
        || options.cursorBlinkVisible != d->options.cursorBlinkVisible || options.preedit.isEmpty() != d->options.preedit.isEmpty();
    const bool blinkChanged{options.textBlinkVisible != d->options.textBlinkVisible};
    const bool positionChanged{options.contentRect != d->options.contentRect};
    const bool imagesChanged{fontChanged || terminalChanged || positionChanged || frame.imagesGeneration != d->frame.imagesGeneration};
    const bool preeditChanged = appearanceChanged || cursorChanged || positionChanged || frame.columns != d->frame.columns
        || frame.rows.size() != d->frame.rows.size() || options.preedit != d->options.preedit || options.preeditAttributes != d->options.preeditAttributes;
    d->frame = frame;
    d->options = options;
    QColor background{frame.background};
    background.setAlphaF(qBound(qreal(0), options.backgroundOpacity, qreal(1)));
    setClipRect(d->clip, options.contentRect.intersected(options.rect));

    if (fontChanged) {
        clearChildren(d->defaultBackground);
        clearChildren(d->cellBackground);
        clearChildren(d->cellText);
        clearChildren(d->preedit);
        d->rows.clear();

        if (d->statistics) {
            d->statistics->rasterizedGlyphs = 0;
        }

        d->atlas = std::make_unique<GlyphAtlas>(d->window, d->statistics ? &d->statistics->rasterizedGlyphs : nullptr);
        d->metrics = fontMetrics(options.font, options.devicePixelRatio, options.logicalDpi);
        d->shaper.setMetrics(d->metrics, options.devicePixelRatio);
    }

    const QRectF grid =
        QRectF(options.contentRect.topLeft(), QSizeF(frame.columns * d->metrics.cellSize.width(), frame.rows.size() * d->metrics.cellSize.height()))
            .intersected(options.contentRect)
            .intersected(options.rect);
    const std::array<QRectF, 4> padding = grid.isEmpty()
        ? std::array<QRectF, 4>{options.rect, {}, {}, {}}
        : std::array<QRectF, 4>{QRectF(options.rect.left(), options.rect.top(), options.rect.width(), grid.top() - options.rect.top()),
                                QRectF(options.rect.left(), grid.bottom(), options.rect.width(), options.rect.bottom() - grid.bottom()),
                                QRectF(options.rect.left(), grid.top(), grid.left() - options.rect.left(), grid.height()),
                                QRectF(grid.right(), grid.top(), options.rect.right() - grid.right(), grid.height())};

    for (size_t i{0}; i < padding.size(); ++i) {
        d->background[i]->setRect(padding[i]);
        d->background[i]->setColor(background);
    }

    d->atlas->beginFrame();

    const bool rowOrderChanged{d->rows.size() != frame.rowIdentities.size()
                               || !std::equal(d->rows.cbegin(), d->rows.cend(), frame.rowIdentities.cbegin(), [](const Private::Row &row, const auto &id) {
                                      return row.identity == id;
                                  })};

    if (frame.rowIdentities.size() == frame.rows.size() && !d->rows.isEmpty() && rowOrderChanged) {
        // Ghostty marks the viewport dirty on scroll even when row geometry can be reused.
        QHash<QPair<quint64, quint64>, int> positions;

        for (int i{0}; i < d->rows.size(); ++i) {
            const auto &id = d->rows.at(i).identity;
            positions.insert({id[0], id[1]}, i);
        }

        QVector<Private::Row> reordered(frame.rows.size());
        QVector<bool> used(d->rows.size());
        bool moved{frame.rows.size() != d->rows.size()};

        for (int i{0}; i < frame.rows.size(); ++i) {
            const auto &id = frame.rowIdentities[i];
            const auto found = positions.constFind({id[0], id[1]});

            if (found != positions.cend() && !used[*found]) {
                reordered[i] = d->rows.at(*found);
                used[*found] = true;
                moved |= *found != i;
            }
        }

        int spare{0};

        for (auto &row : reordered) {
            if (row.background) {
                continue;
            }

            while (spare < used.size() && used[spare]) {
                ++spare;
            }

            if (spare < used.size()) {
                row = d->rows.at(spare);
                used[spare++] = true;
                row.version = 0;
            } else {
                row.base = new QSGTransformNode;
                row.background = new QSGTransformNode;
                row.foreground = new QSGTransformNode;
            }

            moved = true;
        }

        if (moved) {
            for (int i{0}; i < d->rows.size(); ++i) {
                const auto &row = d->rows.at(i);
                d->defaultBackground->removeChildNode(row.base);
                d->cellBackground->removeChildNode(row.background);
                d->cellText->removeChildNode(row.foreground);

                if (!used[i]) {
                    delete row.base;
                    delete row.background;
                    delete row.foreground;
                }
            }

            d->rows = std::move(reordered);

            for (const auto &row : std::as_const(d->rows)) {
                d->defaultBackground->appendChildNode(row.base);
                d->cellBackground->appendChildNode(row.background);
                d->cellText->appendChildNode(row.foreground);
            }
        }
    }

    while (d->rows.size() > frame.rows.size()) {
        const auto row = d->rows.takeLast();
        d->defaultBackground->removeChildNode(row.base);
        d->cellBackground->removeChildNode(row.background);
        d->cellText->removeChildNode(row.foreground);
        delete row.base;
        delete row.background;
        delete row.foreground;
    }

    while (d->rows.size() < frame.rows.size()) {
        Private::Row row;
        row.base = new QSGTransformNode;
        row.background = new QSGTransformNode;
        row.foreground = new QSGTransformNode;
        d->defaultBackground->appendChildNode(row.base);
        d->cellBackground->appendChildNode(row.background);
        d->cellText->appendChildNode(row.foreground);
        d->rows += row;
    }

    bool cursorContentChanged{false};
    auto highlight = options.textHighlights.cbegin();

    for (int y{0}; y < d->rows.size(); ++y) {
        auto &row = d->rows[y]; // clazy:exclude=detaching-member
        const quint64 version{y < frame.rowVersions.size() ? frame.rowVersions[y] : frame.generation};
        const int cursorColumn{d->cursorColumn(y)};
        const bool cursorRowChanged{row.cursorColumn != cursorColumn || (cursorChanged && cursorColumn >= 0)};
        const QRegion matches{options.searchMatches.intersected(QRect(0, y, int(frame.rows[y].size()), 1)).translated(0, -y)};
        const QRegion link{options.hoveredLink.intersected(QRect(0, y, int(frame.rows[y].size()), 1)).translated(0, -y)};
        QList<TextHighlight> highlights;

        for (; highlight != options.textHighlights.cend() && highlight->cells.top() <= y; ++highlight) {
            if (highlight->cells.top() == y) {
                auto local = *highlight;
                local.cells.translate(0, -y);
                highlights += local;
            }
        }

        if (appearanceChanged || cursorRowChanged || (blinkChanged && row.blink) || row.searchMatches != matches || row.hoveredLink != link
            || row.textHighlights != highlights
            || ((version != row.version || row.cells.isEmpty()) && row.cells != frame.rows[y])) {
            row.searchMatches = matches;
            row.hoveredLink = link;
            row.textHighlights = std::move(highlights);
            d->rebuildRow(row, frame.rows[y], cursorColumn);
            cursorContentChanged |= y == frame.cursor.y();
        }

        row.version = version;

        if (y < frame.rowIdentities.size()) {
            row.identity = frame.rowIdentities[y];
        }

        QMatrix4x4 transform;
        transform.translate(options.contentRect.x(), options.contentRect.y() + y * d->metrics.cellSize.height());

        for (auto *node : {row.base, row.background, row.foreground}) {
            if (node->matrix() != transform) {
                node->setMatrix(transform);
            }
        }
    }

    if (imagesChanged) {
        d->rebuildImages();
    }

    if (cursorChanged || cursorContentChanged || appearanceChanged || positionChanged) {
        d->rebuildCursor();
    }

    if (preeditChanged) {
        d->rebuildPreedit();
    }

    d->atlas->trim();

    if (d->atlas->byteSize() > d->compactionThreshold) {
        // Visible glyphs can pin mostly empty atlas pages. Repack their cached coverage,
        // allowing the visible set to exceed the nominal budget.
        d->atlas = std::make_unique<GlyphAtlas>(d->window, d->statistics ? &d->statistics->rasterizedGlyphs : nullptr, std::move(d->atlas));
        d->atlas->beginFrame();

        for (int y{0}; y < d->rows.size(); ++y) {
            d->rebuildRow(d->rows[y], frame.rows[y], d->cursorColumn(y));
        }

        d->rebuildCursor();
        d->rebuildPreedit();
        d->compactionThreshold = qMax(GlyphAtlas::PixelBudget, d->atlas->byteSize() * 3 / 2);
    }

    d->atlas->flush(d->statistics ? &d->statistics->textureUploadBytes : nullptr);

    if (d->statistics) {
        d->statistics->atlasPages = d->atlas->pageCount();
        d->statistics->cachedRuns = d->shaper.cacheSize();
        d->statistics->atlasBytes = d->atlas->byteSize();
        d->statistics->frameGeneration = frame.generation;
        d->statistics->prepareNanoseconds = timer.nsecsElapsed();
    }

    d->initialized = true;
}

TerminalNode::Statistics TerminalNode::statistics() const
{
    return d->statistics ? *d->statistics : Statistics{};
}
}
