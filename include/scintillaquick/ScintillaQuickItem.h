// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file ScintillaQuick_item.h - Qt Quick item (QQuickItem) that wraps
// ScintillaQuick_core and drives events, input method handling and
// scene-graph rendering. This is NOT a QWidget; earlier revisions of
// upstream Scintilla-Qt were widget-based and the comment used to say
// "Qt widget" - the current integration is fully Qt Quick / scene-graph
// native.


#ifndef SCINTILLAQUICK_SCINTILLAQUICKITEM_H
#define SCINTILLAQUICK_SCINTILLAQUICKITEM_H

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Public header only pulls the Scintilla *public* API surface
// (third_party/scintilla/include). Internal Scintilla headers such as
// Debugging.h, Geometry.h and Platform.h live under
// third_party/scintilla/src and are NOT required for the types referenced
// below (Scintilla::Position, NotificationData, Message, uptr_t, sptr_t,
// Update, ModificationFlags, FoldLevel, KeyMod). Keeping them out of the
// public header removes ~3 internal includes from every consumer TU and
// lets the install set drop `third_party/scintilla/src`.
#include "Scintilla.h"
#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"

#include <QFont>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPoint>
#include <QQuickItem>
#include <QTimer>
#include <QVariant>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QSGNode;
class QTouchEvent;
class QWheelEvent;

namespace scintillaquick {

// Library version. Keep in sync with the CMake project() VERSION.
inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

} // namespace scintillaquick

namespace Scintilla::Internal {

class ScintillaQuick_core;
class Surface_impl;
struct Render_frame;

// Test / benchmark support type. Consumed by the in-tree benchmark harness
// and by test_support/ScintillaQuick_validation_access.h. Not intended as
// part of the long-term public API; treat as internal.
struct Displayed_row_for_test
{
    int document_line = 0;
    int subline_index = 0;
    double top        = 0.0;
    double bottom     = 0.0;
    QString text;
};

#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
class ScintillaQuick_validation_access;
#endif

}

#ifndef SCINTILLAQUICK_EXPORT
#if defined(SCINTILLAQUICK_STATIC_DEFINE)
#define SCINTILLAQUICK_EXPORT
#elif defined(WIN32)
#ifdef MAKING_LIBRARY
#define SCINTILLAQUICK_EXPORT __declspec(dllexport)
#else
// Defining dllimport upsets moc
#define SCINTILLAQUICK_EXPORT __declspec(dllimport)
#endif
#else
#define SCINTILLAQUICK_EXPORT
#endif
#endif

// REMARK:
// In Qt QML/Quick modus the scrollbar handling should be handled outside
// the scintilla editor control, for example in a ScrolView component.
// This is needed to optimize the user interaction on touch devices.
// In this modus the scintilla editor control runs alway with a (maximal)
// surface area to show the control completely. Rendering is handled through the
// Qt Quick scene graph in updatePaintNode().
class SCINTILLAQUICK_EXPORT ScintillaQuick_item : public QQuickItem
{
    Q_OBJECT

    Q_PROPERTY(QString text READ getText WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QFont font READ getFont WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(bool readonly READ getReadonly WRITE setReadonly NOTIFY readonlyChanged)
    Q_PROPERTY(int logicalWidth READ getLogicalWidth NOTIFY logicalWidthChanged)
    Q_PROPERTY(int logicalHeight READ getLogicalHeight NOTIFY logicalHeightChanged)
    Q_PROPERTY(int charHeight READ getCharHeight NOTIFY charHeightChanged)
    Q_PROPERTY(int charWidth READ getCharWidth NOTIFY charWidthChanged)
    Q_PROPERTY(int totalLines READ getTotalLines NOTIFY totalLinesChanged)
    Q_PROPERTY(int totalColumns READ getTotalColumns NOTIFY totalColumnsChanged)
    Q_PROPERTY(int visibleLines READ getVisibleLines NOTIFY visibleLinesChanged)
    Q_PROPERTY(int visibleColumns READ getVisibleColumns NOTIFY visibleColumnsChanged)
    Q_PROPERTY(int firstVisibleLine READ getFirstVisibleLine WRITE setFirstVisibleLine NOTIFY firstVisibleLineChanged)
    Q_PROPERTY(int firstVisibleColumn READ getFirstVisibleColumn NOTIFY firstVisibleColumnChanged)
    Q_PROPERTY(bool profilingActive READ profilingActive NOTIFY profilingActiveChanged)
    Q_PROPERTY(Qt::InputMethodHints inputMethodHints READ inputMethodHints WRITE setInputMethodHints NOTIFY inputMethodHintsChanged)

public:
    explicit ScintillaQuick_item(QQuickItem *parent = nullptr);
    virtual ~ScintillaQuick_item();

    virtual sptr_t send(
        unsigned int i_message,
        uptr_t w_param = 0,
        sptr_t l_param = 0) const;

    virtual sptr_t sends(
        unsigned int i_message,
        uptr_t w_param = 0,
        const char *s = 0) const;

    Q_INVOKABLE void scrollRow(int delta_lines);
    Q_INVOKABLE void scrollColumn(int delta_columns);
    Q_INVOKABLE void enableUpdate(bool enable);
    Q_INVOKABLE virtual void cmdContextMenu(int menu_id);
    Q_INVOKABLE bool startProfilingSession(
	    const QString &output_directory = QString(), double duration_seconds = 10.0);
    Q_INVOKABLE void stopProfilingSession();
    Q_INVOKABLE bool profilingActive() const;
    void request_scene_graph_update(
        bool static_content_dirty = false,
        bool needs_style_sync     = false,
        bool scrolling            = false);

public slots:
    // Scroll events coming from GUI to be sent to Scintilla.
    void scrollHorizontal(int value);
    void scrollVertical(int value);

    // Emit Scintilla notifications as signals.
    void notifyParent(Scintilla::NotificationData scn);
    void event_command(Scintilla::uptr_t w_param, Scintilla::sptr_t l_param);

signals:
    void cursorPositionChanged();
    void horizontalScrolled(int value);
    void verticalScrolled(int value);
    void horizontalRangeChanged(int max, int page);
    void verticalRangeChanged(int max, int page);
    void notifyChange();
    void linesAdded(Scintilla::Position linesAdded);

    // Clients can use this hook to add additional
    // formats (e.g. rich text) to the MIME data.
    void aboutToCopy(QMimeData *data);

    // Scintilla Notifications
    void styleNeeded(Scintilla::Position position);
    void charAdded(int ch);
    void savePointChanged(bool dirty);
    void modifyAttemptReadOnly();
    void key(int key);
    void doubleClick(Scintilla::Position position, Scintilla::Position line);
    void updateUi(Scintilla::Update updated);
    void modified(
        Scintilla::ModificationFlags type,
        Scintilla::Position position,
        Scintilla::Position length,
        Scintilla::Position linesAdded,
        const QByteArray& text,
        Scintilla::Position line,
        Scintilla::FoldLevel foldNow,
        Scintilla::FoldLevel foldPrev);
    void macroRecord(
        Scintilla::Message message,
        Scintilla::uptr_t w_param,
        Scintilla::sptr_t l_param);
    void marginClicked(Scintilla::Position position, Scintilla::KeyMod modifiers, int margin);
    void textAreaClicked(Scintilla::Position line, int modifiers);
    void needShown(Scintilla::Position position, Scintilla::Position length);
    void painted();
    void userListSelection(); // Wants some args.
    void uriDropped(const QString &uri);
    void dwellStart(int x, int y);
    void dwellEnd(int x, int y);
    void zoom(int zoom);
    void hotSpotClick(Scintilla::Position position, Scintilla::KeyMod modifiers);
    void hotSpotDoubleClick(Scintilla::Position position, Scintilla::KeyMod modifiers);
    void callTipClick();
    void autoCompleteSelection(Scintilla::Position position, const QString &text);
    void autoCompleteCancelled();
    void focusChanged(bool focused);

    // Base Scintilla notifications exposed by this item.
    void notify(Scintilla::NotificationData *pscn);
    void command(Scintilla::uptr_t w_param, Scintilla::sptr_t l_param);

    // GUI event notifications needed under Qt
    void buttonPressed(QMouseEvent *event);
    void buttonReleased(QMouseEvent *event);
    void keyPressed(QKeyEvent *event);
    void resized();
    void textChanged();
    void fontChanged();
    void readonlyChanged();
    void profilingActiveChanged();
    void profilingFinished(const QString &reportPath);
    void logicalWidthChanged();
    void logicalHeightChanged();
    void charHeightChanged();
    void charWidthChanged();
    void totalLinesChanged();
    void firstVisibleLineChanged();
    void firstVisibleColumnChanged();
    void totalColumnsChanged();
    void visibleLinesChanged();
    void visibleColumnsChanged();
    void inputMethodHintsChanged();
    void enableScrollViewInteraction(bool value);
    void showContextMenu(const QPoint & pos);
    void addToContextMenu(int menuId, const QString & txt, bool enabled);
    void clearContextMenu();

protected:
    bool event(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery property, QVariant argument) const;
    void touchEvent(QTouchEvent *event) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *update_paint_node_data) override;

private:
#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
    friend class Scintilla::Internal::ScintillaQuick_validation_access;
#endif

    class Render_data;
    class Profiling_state;

    QString getText() const;
    void setText(const QString & txt);
    QFont getFont() const { return m_font; }
    void setFont(const QFont & newFont);
    void setStylesFont(const QFont &f, int style);
    int getLogicalWidth() const;
    int getLogicalHeight() const;
    int getCharHeight() const;
    int getCharWidth() const;
    int getFirstVisibleLine() const;
    void setFirstVisibleLine(int lineNo);
    int getTotalLines() const;
    int getFirstVisibleColumn() const;
    int getTotalColumns() const;
    int getVisibleLines() const;
    int getVisibleColumns() const;
    Qt::InputMethodHints inputMethodHints() const;
    void setInputMethodHints(Qt::InputMethodHints hints);
    bool getReadonly() const;
    void setReadonly(bool value);

    void cursorChangedUpdateMarker();
    void syncCaretBlinkTimer(bool resetPhase = false);
    void updateQuickView(Scintilla::Update updated);
    void build_render_snapshot();
    std::vector<Scintilla::Internal::Displayed_row_for_test> displayed_rows_for_test() const;
    const Scintilla::Internal::Render_frame &rendered_frame_for_test() const;
    void reset_tracked_scroll_width();

    bool m_updates_enabled;
    int m_logical_width;
    int m_logical_height;
    // The following members are NOT a read cache - `getCharHeight()`,
    // `getCharWidth()`, etc. re-query Scintilla on every call. They
    // are the "last value we emitted a NOTIFY signal for" so that
    // `syncQuickViewProperties()` can avoid spurious property-change
    // notifications when nothing has actually changed between two
    // consecutive sync passes. Name reflects the role.
    int m_last_emitted_char_height          = -1;
    int m_last_emitted_char_width           = -1;
    int m_last_emitted_total_lines          = -1;
    int m_last_emitted_total_columns        = -1;
    int m_last_emitted_visible_lines        = -1;
    int m_last_emitted_visible_columns      = -1;
    int m_last_emitted_first_visible_line   = -1;
    int m_last_emitted_first_visible_column = -1;
    QFont m_font;
    Qt::InputMethodHints m_input_method_hints;
    qint64 m_last_touch_press_time;

    Scintilla::Internal::ScintillaQuick_core *m_core;

    QElapsedTimer m_elapsed_timer;

    Scintilla::Position m_preedit_pos;
    std::unique_ptr<Render_data> m_render_data;
    std::unique_ptr<Profiling_state> m_profiling_state;
    QTimer m_caret_blink_timer;
    bool m_caret_blink_visible = true;
    // Re-entry guard for `send()`'s dispatch -> `syncQuickViewProperties()`
    // path. `syncQuickViewProperties()` itself issues SCI_* queries
    // through `send()` to read the geometry cache (SCI_TEXTHEIGHT /
    // SCI_LINESONSCREEN / ...). If a query message is not in the
    // `scene_graph_message_is_known_read_only()` allow-list, the
    // dispatch's conservative "unknown -> full resync" default would
    // call `syncQuickViewProperties()` again, causing unbounded
    // recursion and a stack overflow. The allow-list in the dispatch
    // table is the primary defence; this flag is a defence-in-depth so
    // that a future missed entry degrades into "no resync for that one
    // nested call" instead of a crash.
    //
    // Declared mutable because `send()` is const (see the long comment
    // at the top of `send()` for why).
    mutable bool m_in_sync_quick_view_properties = false;

    static bool IsHangul(const QChar qchar);
    void MoveImeCarets(Scintilla::Position offset);
    void DrawImeIndicator(int indicator, int len);
    static Scintilla::KeyMod ModifiersOfKeyboard();
    void syncQuickViewProperties();
};

void RegisterScintillaType();

#endif
