// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "Session.h"
#include "TextHighlighter_p.h"
#include "kwiterminal_export.h"
#include <QColor>
#include <QFont>
#include <QHash>
#include <QInputMethodEvent>
#include <QList>
#include <QMetaObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QRegion>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

class QScreen;
class KDirWatch;
class KModifierKeyInfo;

namespace Kwi
{
class TerminalSearch;
struct TerminalColors;

/*! \class Kwi::TerminalView
 * \internal
 */

/*!
 * \qmltype TerminalView
 * \inqmlmodule org.kde.kwi.terminal
 * \inherits Item
 * \brief Displays a session and handles terminal input.
 *
 * Set \l session to display a terminal. TerminalView does not create or start a
 * process. Its size determines the terminal's columns and rows, and it accepts
 * keyboard input when it has \l {Item::activeFocus}{active focus}.
 *
 * \l Terminal adds a scrollbar, a visual bell and a keyboard selection cursor
 * around this view. Use TerminalView directly when providing those controls
 * yourself. The embedding application handles context menus, link activation
 * and any required paste confirmation through the corresponding signals.
 */
class KWI_TERMINAL_EXPORT TerminalView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    /*!
     * \qmlproperty Session TerminalView::session
     * The session to display. The default is null. A session can be attached to
     * only one TerminalView at a time. Set the old view's session to null before
     * attaching it to another view. An assignment that violates this rule is ignored.
     *
     * The view does not own or close the session. Destroying or detaching the view
     * leaves the process running.
     */
    Q_PROPERTY(Kwi::Session *session READ session WRITE setSession NOTIFY sessionChanged)
    /*!
     * \qmlproperty string TerminalView::fontFamily
     * The terminal font family. By default it follows the system fixed-width font.
     * Assign an empty string to restore that behavior. Reading this property
     * returns the configured family or the system fixed-width family.
     */
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    /*!
     * \qmlproperty real TerminalView::fontPointSize
     * The font size in points, before zoom. By default it follows the system
     * fixed-width font size. Assign zero to restore that behavior. Reading this
     * property returns the effective base size.
     *
     * Nonzero values are clamped to 6 through 48 points. Non-finite values are ignored.
     *
     * \sa zoomIn(), zoomOut(), resetZoom()
     */
    Q_PROPERTY(qreal fontPointSize READ fontPointSize WRITE setFontPointSize NOTIFY fontChanged)
    /*!
     * \qmlproperty string TerminalView::colorScheme
     * The terminal color scheme identifier. The default, \c system, follows the
     * application palette's text and base colors. ANSI colors remain a separate
     * terminal palette.
     *
     * \table
     * \header \li Identifier \li Source
     * \row \li \c system \li The application palette.
     * \row \li \c dark \li The bundled Breeze scheme.
     * \row \li \c light \li The bundled Black on White scheme.
     * \row \li \c {konsole:<filename>.colorscheme} \li A bundled scheme or one installed under \c konsole in the standard data locations.
     * \row \li \c {imported:<filename>.colorscheme} \li A file under \c kwi/color-schemes in the user's data directory.
     * \row \li \c {user:<filename>.colorscheme} \li A file under \c kwi/color-schemes/user in the user's data directory.
     * \endtable
     *
     * Filenames cannot contain directory separators. An unavailable or invalid
     * scheme falls back to system colors. Changes to the application palette and
     * a loaded scheme file are applied while the view is open.
     *
     * \sa foregroundColor, backgroundColor
     */
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY appearanceChanged)
    /*!
     * \qmlproperty color TerminalView::foregroundColor
     * \readonly
     * The displayed frame's default foreground color, including color overrides
     * from the terminal process. It is invalid before a frame is available.
     */
    Q_PROPERTY(QColor foregroundColor READ foregroundColor NOTIFY colorsChanged)
    /*!
     * \qmlproperty color TerminalView::backgroundColor
     * \readonly
     * The displayed frame's default background color, including color overrides
     * from the terminal process. It is invalid before a frame is available.
     * This color does not include backgroundOpacity.
     */
    Q_PROPERTY(QColor backgroundColor READ backgroundColor NOTIFY colorsChanged)
    /*!
     * \qmlproperty real TerminalView::backgroundOpacity
     * The opacity of the terminal's default background, from 0 to 1. The default
     * is 1. Text and explicitly colored cells stay opaque. Values are clamped to
     * this range and non-finite values are ignored.
     *
     * The embedding window must support transparency for content behind the window
     * to show through. This property does not request compositor blur.
     */
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY backgroundOpacityChanged)
    /*!
     * \qmlproperty string TerminalView::cursorStyle
     * The default cursor shape: \c block, \c beam or \c underline.
     * The default is \c block. Other values are ignored. The terminal process
     * can override the shape through escape sequences.
     */
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY appearanceChanged)
    /*!
     * \qmlproperty bool TerminalView::cursorBlinking
     * Whether the cursor blinks by default. The default is false. The terminal
     * process can override this through escape sequences.
     */
    Q_PROPERTY(bool cursorBlinking READ cursorBlinking WRITE setCursorBlinking NOTIFY appearanceChanged)
    /*!
     * \qmlproperty real TerminalView::contentPadding
     * The space between the terminal grid and the view's edges, in logical
     * pixels. The default is 1. Values are clamped to 0 to 64 and rounded to
     * whole physical pixels. The padding uses the terminal's default background.
     */
    Q_PROPERTY(qreal contentPadding READ contentPadding WRITE setContentPadding NOTIFY appearanceChanged)
    /*!
     * \qmlproperty bool TerminalView::syntaxHighlighting
     * Whether common tokens in terminal output, such as log levels, URLs, paths,
     * addresses, dates and numbers, are colored using the color scheme's ANSI
     * palette. Only text drawn in the default foreground color is affected, so
     * colors chosen by the terminal process are preserved. Highlighting does not
     * change the terminal contents. The default is false.
     */
    Q_PROPERTY(bool syntaxHighlighting READ syntaxHighlighting WRITE setSyntaxHighlighting NOTIFY appearanceChanged)
    /*!
     * \qmlproperty bool TerminalView::confirmMultilinePaste
     * Whether a paste containing a newline or carriage return emits
     * multilinePasteRequested() instead of being sent immediately. The default
     * is true. The view does not provide a confirmation dialog.
     *
     * \sa requestPaste(), Session::pasteApprovedText()
     */
    Q_PROPERTY(bool confirmMultilinePaste MEMBER m_confirmMultilinePaste NOTIFY confirmMultilinePasteChanged)
    /*!
     * \qmlproperty bool TerminalView::copyOnSelect
     * Whether completing a mouse selection copies it to the clipboard. The default
     * is false. The primary selection is updated regardless of this setting on
     * platforms that support it.
     */
    Q_PROPERTY(bool copyOnSelect MEMBER m_copyOnSelect NOTIFY copyOnSelectChanged)
    /*!
     * \qmlproperty bool TerminalView::requireCtrlForLinks
     * Whether activating a link with the left mouse button requires Ctrl. The
     * default is true. When false, hovering a link also changes the cursor without
     * Ctrl. Applications that use mouse tracking still receive unmodified clicks.
     *
     * Link activation emits linkActivated(). It does not open the URL itself.
     */
    Q_PROPERTY(bool requireCtrlForLinks READ requireCtrlForLinks WRITE setRequireCtrlForLinks NOTIFY requireCtrlForLinksChanged)

    /*!
     * \qmlproperty string TerminalView::selectedText
     * \readonly
     * The selected terminal text, or an empty string when nothing is selected.
     */
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)
    /*!
     * \qmlproperty bool TerminalView::hasSelection
     * \readonly
     * Whether the terminal has a selection. A selection can contain no text.
     */
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    /*!
     * \qmlproperty string TerminalView::searchStatus
     * \readonly
     * A translated search result summary or error message, ready for display.
     * It is empty before a search and after clearSearch(). The previous summary
     * can remain visible while a new search is in progress.
     *
     * Use searchBusy and searchMatchCount for application logic rather than parsing
     * this text.
     */
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchStatusChanged)
    /*!
     * \qmlproperty bool TerminalView::searchBusy
     * \readonly
     * Whether a search is still processing terminal contents. Results can be
     * available before processing finishes.
     */
    Q_PROPERTY(bool searchBusy READ isSearchBusy NOTIFY searchStatusChanged)
    /*!
     * \qmlproperty int TerminalView::searchMatchCount
     * \readonly
     * The number of matches found, or -1 when no result is available. It can be
     * partial or retain the previous count while searchBusy is true.
     */
    Q_PROPERTY(qsizetype searchMatchCount READ searchMatchCount NOTIFY searchStatusChanged)

    /*!
     * \qmlproperty real TerminalView::scrollPosition
     * \readonly
     * The viewport's top row as a fraction of all retained rows, including the
     * screen. Zero is the top of history. The bottom position is \c {1 - scrollSize}.
     * Returns zero when no frame is available.
     *
     * \sa scrollTo(), scrollSize
     */
    Q_PROPERTY(qreal scrollPosition READ scrollPosition NOTIFY scrollChanged)
    /*!
     * \qmlproperty real TerminalView::scrollSize
     * \readonly
     * The fraction of retained rows visible in the viewport. It is 1 when there
     * is no scrollable history or no frame is available. This can be bound to a
     * scrollbar's size.
     */
    Q_PROPERTY(qreal scrollSize READ scrollSize NOTIFY scrollChanged)
    /*!
     * \qmlproperty real TerminalView::scrollStepSize
     * \readonly
     * One row as a fraction of all retained rows, for use as a scrollbar step size.
     * It is 1 when no frame is available.
     */
    Q_PROPERTY(qreal scrollStepSize READ scrollStepSize NOTIFY scrollChanged)
    /*!
     * \qmlproperty url TerminalView::hoveredLink
     * \readonly
     * The link under the pointer, or an empty URL. This property identifies links
     * even when requireCtrlForLinks prevents an unmodified click from activating them.
     */
    Q_PROPERTY(QUrl hoveredLink READ hoveredLink NOTIFY hoveredLinkChanged)

    /*!
     * \qmlproperty bool TerminalView::keyboardSelectionMode
     * Whether keys move a selection cursor instead of being sent to the process.
     * The default is false. Enabling it selects the cell at the terminal cursor
     * and focuses the view. It cannot be enabled before a terminal frame is available.
     *
     * Arrow keys, Home, End, Page Up and Page Down move the cursor. Shift extends
     * the selection. Ctrl+Home and Ctrl+End move to the start and end of retained
     * text. Enter or Ctrl+C copies and leaves this mode. Escape leaves the mode
     * without copying.
     *
     * \sa selectionCursorRectangle, toggleSelectionMode()
     */
    Q_PROPERTY(bool keyboardSelectionMode READ keyboardSelectionMode WRITE setKeyboardSelectionMode NOTIFY keyboardSelectionChanged)
    /*!
     * \qmlproperty rect TerminalView::selectionCursorRectangle
     * \readonly
     * The keyboard selection cursor's rectangle in view coordinates. Use it only
     * while keyboardSelectionMode is true. \l Terminal uses this to draw the cursor.
     */
    Q_PROPERTY(QRectF selectionCursorRectangle READ selectionCursorRectangle NOTIFY keyboardSelectionChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    [[nodiscard]] Session *session() const;
    void setSession(Session *newSession);

    [[nodiscard]] QString fontFamily() const;
    void setFontFamily(const QString &family);
    [[nodiscard]] qreal fontPointSize() const;
    void setFontPointSize(qreal size);
    /*!
     * \qmlmethod size TerminalView::sizeForGrid(int columns, int rows)
     * Returns the view size needed for \a columns and \a rows with the current
     * font, zoom and screen scale, including padding. It does not resize the view.
     * Fewer than two columns or one row returns an invalid size.
     */
    [[nodiscard]] Q_INVOKABLE QSizeF sizeForGrid(int columns, int rows) const;
    [[nodiscard]] QString colorScheme() const;
    void setColorScheme(const QString &scheme);
    [[nodiscard]] QColor foregroundColor() const;
    [[nodiscard]] QColor backgroundColor() const;
    [[nodiscard]] qreal backgroundOpacity() const;
    void setBackgroundOpacity(qreal opacity);
    [[nodiscard]] QString cursorStyle() const;
    void setCursorStyle(const QString &style);
    [[nodiscard]] bool cursorBlinking() const;
    void setCursorBlinking(bool blinking);
    [[nodiscard]] qreal contentPadding() const;
    void setContentPadding(qreal padding);
    [[nodiscard]] bool syntaxHighlighting() const;
    void setSyntaxHighlighting(bool enabled);
    [[nodiscard]] bool requireCtrlForLinks() const;
    void setRequireCtrlForLinks(bool required);

    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] QString searchStatus() const;
    [[nodiscard]] bool isSearchBusy() const;
    [[nodiscard]] qsizetype searchMatchCount() const;

    [[nodiscard]] qreal scrollPosition() const;
    [[nodiscard]] qreal scrollSize() const;
    [[nodiscard]] qreal scrollStepSize() const;
    [[nodiscard]] QUrl hoveredLink() const;

    [[nodiscard]] bool keyboardSelectionMode() const;
    void setKeyboardSelectionMode(bool enabled);
    [[nodiscard]] QRectF selectionCursorRectangle() const;
    /*!
     * \qmlmethod void TerminalView::toggleSelectionMode()
     * Toggles keyboardSelectionMode.
     */
    Q_INVOKABLE void toggleSelectionMode();

    /*!
     * \qmlmethod list<var> TerminalView::links()
     * Returns the links in the visible viewport, with duplicate URLs removed.
     * Each entry has a \c url and a \c label string. This includes both explicit
     * terminal hyperlinks and links detected in text.
     */
    [[nodiscard]] Q_INVOKABLE QVariantList links() const;
    /*!
     * \qmlmethod void TerminalView::copyLink(url url)
     * Copies \a url to the clipboard as a fully encoded URL. An invalid URL
     * or one without a scheme leaves the clipboard unchanged.
     */
    Q_INVOKABLE void copyLink(const QUrl &url);

    /*!
     * \qmlmethod void TerminalView::copy()
     * Copies selectedText to the clipboard. An empty selection leaves the
     * clipboard unchanged.
     */
    Q_INVOKABLE void copy();
    /*!
     * \qmlmethod bool TerminalView::copyCommandOutput()
     * Copies the latest command's output to the clipboard, using shell integration
     * markers to identify it. Returns true if nonempty output was copied.
     * Otherwise returns false and leaves the clipboard unchanged.
     */
    Q_INVOKABLE bool copyCommandOutput();
    /*!
     * \qmlmethod void TerminalView::paste()
     * Pastes the clipboard text through requestPaste().
     */
    Q_INVOKABLE void paste();
    /*!
     * \qmlmethod void TerminalView::pasteSelection()
     * Pastes the primary selection through requestPaste(). The platform must
     * support primary selection.
     */
    Q_INVOKABLE void pasteSelection();
    /*!
     * \qmlmethod void TerminalView::requestPaste(string text)
     * Applies the view's paste policy to \a text. When confirmMultilinePaste is true
     * and the text contains a line break, emits multilinePasteRequested(). Otherwise
     * passes it to Session::pasteApprovedText().
     *
     * Does nothing without a session or when Session::readOnly is true.
     */
    Q_INVOKABLE void requestPaste(const QString &text);
    /*!
     * \qmlmethod void TerminalView::selectAll()
     * Selects all retained terminal text and leaves keyboardSelectionMode.
     */
    Q_INVOKABLE void selectAll();

    /*!
     * \qmlmethod void TerminalView::find(string text, bool forward = true)
     * Searches the retained terminal text for the literal string \a text.
     * ASCII letters match case-insensitively. Other characters must match exactly.
     * A true \a forward selects the next match, false the previous
     * match. Repeated calls with the same text move between matches and wrap at
     * the ends. Selecting a match scrolls it into view.
     *
     * Search results update as terminal contents change. Observe searchBusy,
     * searchMatchCount and searchStatus for progress and results. An empty search
     * string calls clearSearch().
     */
    Q_INVOKABLE void find(const QString &text, bool forward = true);
    /*!
     * \qmlmethod void TerminalView::clearSearch()
     * Clears the search, match highlighting and terminal selection, and leaves
     * keyboardSelectionMode. The viewport stays at its current position.
     */
    Q_INVOKABLE void clearSearch();

    /*!
     * \qmlmethod void TerminalView::zoomIn()
     * Increases the session's font size by one point, up to 72 points.
     * The base fontPointSize is unchanged.
     */
    Q_INVOKABLE void zoomIn();
    /*!
     * \qmlmethod void TerminalView::zoomOut()
     * Decreases the session's font size by one point, down to 6 points.
     * The base fontPointSize is unchanged.
     */
    Q_INVOKABLE void zoomOut();
    /*!
     * \qmlmethod void TerminalView::resetZoom()
     * Restores the session's font size to fontPointSize.
     */
    Q_INVOKABLE void resetZoom();

    /*!
     * \qmlmethod void TerminalView::scrollTo(real position)
     * Moves the viewport to \a position, expressed in the same units as
     * scrollPosition. Zero moves to the top of history. Values at or beyond
     * \c {1 - scrollSize} move to the bottom. Non-finite values are ignored.
     */
    Q_INVOKABLE void scrollTo(qreal position);
    /*!
     * \qmlmethod void TerminalView::scrollHistory(point pixelDelta, point angleDelta, int modifiers = Qt.NoModifier)
     * Scrolls history using a wheel event's \a pixelDelta, \a angleDelta and
     * keyboard \a modifiers. Positive vertical deltas move toward older output.
     * Angle deltas take precedence and use the system's wheel scroll distance.
     * Pixel deltas are used when angleDelta is zero.
     *
     * Shift scrolls by pages, limited to one page per event. This method always
     * scrolls history, even when the process requests mouse events.
     */
    Q_INVOKABLE void scrollHistory(QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

Q_SIGNALS:
    void sessionChanged();
    void fontChanged();
    void appearanceChanged();
    void colorsChanged();
    void backgroundOpacityChanged();
    void confirmMultilinePasteChanged();
    void copyOnSelectChanged();
    void requireCtrlForLinksChanged();
    void selectionChanged();
    void searchStatusChanged();
    void scrollChanged();
    /*!
     * \qmlsignal TerminalView::multilinePasteRequested(Session origin, string text)
     * Emitted when a paste of \a text needs confirmation. If approved, call
     * \l {Session::pasteApprovedText()}{origin.pasteApprovedText(text)}. Keep the
     * request associated with \a origin even if the view switches to another session
     * while the confirmation is open. Do not paste if that session has been destroyed.
     */
    void multilinePasteRequested(Kwi::Session *origin, const QString &text);
    /*!
     * \qmlsignal TerminalView::contextMenuRequested(point position)
     * Emitted when a context menu is requested at \a position in view coordinates.
     * The request can come from the mouse or keyboard. The embedding application
     * creates and opens the menu.
     */
    void contextMenuRequested(const QPointF &position);
    void hoveredLinkChanged();
    /*!
     * \qmlsignal TerminalView::linkActivated(url url)
     * Emitted when the user activates a link with target \a url. The embedding
     * application decides how to open it.
     */
    void linkActivated(const QUrl &url);
    void keyboardSelectionChanged();
    /*!
     * \qmlsignal TerminalView::focusRequested()
     * Emitted when an accessibility client requests focus. The embedding
     * application can reveal the surrounding UI before the view takes focus.
     */
    void focusRequested();

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    static constexpr qreal Padding{1};

    void changeSession(Session *newSession);
    int wheelSteps(QPoint pixelDelta, QPoint angleDelta, int stepsPerNotch);

    friend class TerminalAccessible;
    static void installAccessibility();
    void updateAccessibility();

    void refresh();
    void reloadColors();
    void applyColors(const TerminalColors &colors);
    void updateMetrics();
    void watchScreen(QScreen *screen);
    void resizeTerminal();
    void updateCursorDefaults();
    void updateBlinkTimers();
    void updateTextHighlights();
    void resetCursorBlink();

    void resetPreedit();
    void encodeKey(QKeyEvent *event);
    void showContextMenu();
    bool handleSelectionKey(QKeyEvent *event);
    [[nodiscard]] QPoint glyphStart(QPoint cell) const;
    [[nodiscard]] QPoint cellAt(QPointF position) const;
    [[nodiscard]] QUrl linkAtPosition(QPointF position, QRegion *cells = nullptr) const;
    bool reportMouse(QMouseEvent *event, bool release, bool motion);
    void sendMouse(GhosttyQt::MouseEvent event);
    void updateSelection(GhosttyQt::SelectionEvent::Type type, QPointF position, quint64 timestamp = 0);
    void updateHoveredLink(std::optional<QPointF> position, Qt::KeyboardModifiers modifiers);
    [[nodiscard]] bool linkActivationAllowed(Qt::KeyboardModifiers modifiers) const;
    void openLink();
    void scrollSelection();

    QPointer<Session> m_session;
    GhosttyQt::Frame m_frame;
    quint64 m_frameRevision{0};
    QFont m_font;
    QString m_fontFamily;
    QString m_colorScheme{QStringLiteral("system")};
    QString m_cursorStyle{QStringLiteral("block")};

    TerminalSearch *m_search;
    QRegion m_searchMatches;
    QList<GhosttyQt::TextHighlight> m_textHighlights;
    QHash<QString, QList<TextHighlightSpan>> m_textHighlightCache;
    QVector<QRgb> m_highlightPalette;
    QString m_preedit;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;

    QTimer m_cursorBlinkTimer;
    QTimer m_textBlinkTimer;
    QTimer m_selectionScrollTimer;

    KModifierKeyInfo *m_modifierKeyInfo;
    KDirWatch *m_paletteWatch;
    QTimer m_paletteReloadTimer;
    QString m_palettePath;

    QMetaObject::Connection m_screenConnection;
    QMetaObject::Connection m_dpiConnection;

    QUrl m_hoveredLink;
    QRegion m_highlightedLink;
    std::optional<QPointF> m_hoverPosition;

    qreal m_cellWidth{10};
    qreal m_cellHeight{20};
    qreal m_contentPadding{Padding};
    qreal m_padding{Padding};
    qreal m_zoom{0};
    qreal m_pointSize{0};
    qreal m_backgroundOpacity{1};

    qreal m_wheelAngleRemainder{0};
    qreal m_wheelPixelRemainder{0};
    qreal m_zoomWheelRemainder{0};
    int m_wheelStepsPerNotch{0};
    QSet<quint64> m_pressedKeys;

    bool m_cursorBlinking{false};
    bool m_syntaxHighlighting{false};
    bool m_cursorBlinkVisible{true};
    bool m_textBlinkVisible{true};
    bool m_hasBlinkingText{false};
    bool m_publishingScroll{false};

    bool m_selecting{false};
    bool m_hadSelection{false};
    bool m_selectionTextDirty{false};
    bool m_selectionRectangle{false};

    // Track consumed presses across focus/grab changes during native drags.
    bool m_mousePressConsumed{false};
    bool m_dragPending{false};
    QPointF m_dragStart;

    bool m_confirmMultilinePaste{true};
    bool m_copyOnSelect{false};
    bool m_requireCtrlForLinks{true};

    bool m_keyboardSelectionMode{false};
    QPoint m_keyboardCursor;
    QPointF m_selectionPointer;
};
}
