// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file scintillaquick_item.h - Qt Quick item (QQuickItem) that wraps
// ScintillaQuick_core and drives events, input method handling, and
// scene-graph rendering.

#ifndef SCINTILLAQUICK_SCINTILLAQUICK_ITEM_H
#define SCINTILLAQUICK_SCINTILLAQUICK_ITEM_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPoint>
#include <QPointer>
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

namespace scintillaquick
{

// Library version. Keep in sync with the CMake project() VERSION.
inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

} // namespace scintillaquick

namespace Scintilla::Internal
{

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

} // namespace Scintilla::Internal

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

enum class ScintillaQuick_lparam_kind {
    None,
    Numeric,
    Text
};

struct SCINTILLAQUICK_EXPORT ScintillaQuick_notification
{
    Scintilla::uptr_t hwndFrom = 0;
    Scintilla::uptr_t idFrom   = 0;
    Scintilla::Notification code = static_cast<Scintilla::Notification>(0);
    Scintilla::Position position = 0;
    int ch = 0;
    Scintilla::KeyMod modifiers = Scintilla::KeyMod::Norm;
    Scintilla::ModificationFlags modificationType = Scintilla::ModificationFlags::None;
    QByteArray text;
    bool textAvailable = false;
    Scintilla::Position length = 0;
    Scintilla::Position linesAdded = 0;
    Scintilla::Message message = static_cast<Scintilla::Message>(0);
    Scintilla::uptr_t wParam = 0;
    ScintillaQuick_lparam_kind lParamKind = ScintillaQuick_lparam_kind::None;
    Scintilla::sptr_t lParamValue = 0;
    QByteArray lParamText;
    bool lParamTextAvailable = false;
    Scintilla::Position line = 0;
    Scintilla::FoldLevel foldLevelNow = Scintilla::FoldLevel::None;
    Scintilla::FoldLevel foldLevelPrev = Scintilla::FoldLevel::None;
    int margin = 0;
    int listType = 0;
    int x = 0;
    int y = 0;
    int token = 0;
    Scintilla::Position annotationLinesAdded = 0;
    Scintilla::Update updated = Scintilla::Update::None;
    Scintilla::CompletionMethods listCompletionMethod =
        static_cast<Scintilla::CompletionMethods>(0);
    Scintilla::CharacterSource characterSource = Scintilla::CharacterSource::DirectInput;
};

Q_DECLARE_METATYPE(ScintillaQuick_notification)
Q_DECLARE_METATYPE(Scintilla::ModificationFlags)
Q_DECLARE_METATYPE(Scintilla::FoldLevel)

class ScintillaQuick_item;

struct SCINTILLAQUICK_EXPORT ScintillaQuick_edit_replacement
{
    Scintilla::Position position       = 0;
    Scintilla::Position deleted_length = 0;
    QByteArray inserted_text;
};

struct SCINTILLAQUICK_EXPORT ScintillaQuick_edit_transaction
{
    std::uint64_t transaction_id = 0;
    // This view is valid only during the synchronous handler call. Replacements
    // are ordered; each position addresses the document produced by the
    // preceding replacement in this transaction.
    std::span<const ScintillaQuick_edit_replacement> replacements;
};

enum class ScintillaQuick_edit_disposition
{
    DECLINED,
    HANDLED,
    REJECTED,
};

struct SCINTILLAQUICK_EXPORT ScintillaQuick_edit_result
{
    ScintillaQuick_edit_disposition disposition = ScintillaQuick_edit_disposition::DECLINED;
    // A HANDLED callback must not throw. An escaping exception terminates the
    // process because the callback may already have applied part of a compound
    // replacement and ScintillaQuick cannot roll that external state back.
    std::function<void(ScintillaQuick_item&)> apply;
};

using ScintillaQuick_edit_handler =
    std::function<ScintillaQuick_edit_result(const ScintillaQuick_edit_transaction&)>;

// Scrollbar interaction is handled by the surrounding Qt Quick container
// rather than by embedding widget-style scrollbars into the editor item
// itself. The item therefore renders the full editor surface and relies on
// `updatePaintNode()` for scene-graph presentation.
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
    Q_PROPERTY(Qt::InputMethodHints inputMethodHints READ inputMethodHints WRITE setInputMethodHints
        NOTIFY inputMethodHintsChanged)
    Q_PROPERTY(bool findPanelVisible READ findPanelVisible WRITE setFindPanelVisible
        NOTIFY findPanelVisibleChanged)
    Q_PROPERTY(bool findReplaceMode READ findReplaceMode WRITE setFindReplaceMode
        NOTIFY findReplaceModeChanged)
    Q_PROPERTY(QString findText READ findText WRITE setFindText NOTIFY findTextChanged)
    Q_PROPERTY(QString replacementText READ replacementText WRITE setReplacementText
        NOTIFY replacementTextChanged)
    Q_PROPERTY(int findOptions READ findOptions WRITE setFindOptions NOTIFY findOptionsChanged)
    Q_PROPERTY(QFont findPanelFont READ findPanelFont WRITE setFindPanelFont
        NOTIFY findPanelFontChanged)
    Q_PROPERTY(QColor findPanelBackgroundColor READ findPanelBackgroundColor
        WRITE setFindPanelBackgroundColor NOTIFY findPanelBackgroundColorChanged)
    Q_PROPERTY(QColor findPanelForegroundColor READ findPanelForegroundColor
        WRITE setFindPanelForegroundColor NOTIFY findPanelForegroundColorChanged)
    Q_PROPERTY(QColor findPanelFieldBackgroundColor READ findPanelFieldBackgroundColor
        WRITE setFindPanelFieldBackgroundColor NOTIFY findPanelFieldBackgroundColorChanged)
    Q_PROPERTY(QColor findPanelFieldForegroundColor READ findPanelFieldForegroundColor
        WRITE setFindPanelFieldForegroundColor NOTIFY findPanelFieldForegroundColorChanged)
    Q_PROPERTY(QColor findPanelBorderColor READ findPanelBorderColor
        WRITE setFindPanelBorderColor NOTIFY findPanelBorderColorChanged)
    Q_PROPERTY(QColor findPanelButtonHoverColor READ findPanelButtonHoverColor
        WRITE setFindPanelButtonHoverColor NOTIFY findPanelButtonHoverColorChanged)
    Q_PROPERTY(QColor findPanelDisabledForegroundColor READ findPanelDisabledForegroundColor
        WRITE setFindPanelDisabledForegroundColor NOTIFY findPanelDisabledForegroundColorChanged)
    Q_PROPERTY(QColor findPanelSelectionBackgroundColor READ findPanelSelectionBackgroundColor
        WRITE setFindPanelSelectionBackgroundColor NOTIFY findPanelSelectionBackgroundColorChanged)
    Q_PROPERTY(QColor findPanelSelectionForegroundColor READ findPanelSelectionForegroundColor
        WRITE setFindPanelSelectionForegroundColor NOTIFY findPanelSelectionForegroundColorChanged)

public:
    explicit ScintillaQuick_item(QQuickItem* parent = nullptr);
    virtual ~ScintillaQuick_item();

    virtual sptr_t send(unsigned int i_message, uptr_t w_param = 0, sptr_t l_param = 0) const;

    virtual sptr_t sends(unsigned int i_message, uptr_t w_param = 0, const char* s = 0) const;

    // The handler receives normalized replacements before the document changes.
    // It covers stream selections without virtual space, ordinary typing and
    // deletion (including overtype), stream clipboard and text-drop operations,
    // committed inline IME, find replacement, and direct text/range replacement
    // messages. Compound operations reuse one nonzero transaction id.
    //
    // A HANDLED result may update this derived editor only through `apply`, which
    // runs synchronously with recursive delegation disabled and must not throw.
    // A DECLINED handler must be side-effect-free; its `apply` callback is ignored
    // and Scintilla's existing operation runs unchanged. With a handler installed,
    // an edit that cannot be normalized is rejected with SC_STATUS_FAILURE. This
    // includes multi/rectangular/virtual-space editing, line paste, autocomplete,
    // undo, and redo.
    void set_edit_handler(ScintillaQuick_edit_handler handler);

    Q_INVOKABLE void scrollRow(int delta_lines);
    Q_INVOKABLE void scrollColumn(int delta_columns);
    Q_INVOKABLE void enableUpdate(bool enable);
    Q_INVOKABLE virtual void cmdContextMenu(int menu_id);
    Q_INVOKABLE void showFind();
    Q_INVOKABLE void showFindReplace();
    Q_INVOKABLE void hideFindPanel();
    Q_INVOKABLE bool findNext();
    Q_INVOKABLE bool findPrevious();
    Q_INVOKABLE int selectAllFindMatches();
    Q_INVOKABLE bool replaceSelection();
    Q_INVOKABLE bool replaceAndFind();
    Q_INVOKABLE int replaceAll();

    bool findPanelVisible() const;
    void setFindPanelVisible(bool visible);
    bool findReplaceMode() const;
    void setFindReplaceMode(bool replace_mode);
    QString findText() const;
    void setFindText(const QString& text);
    QString replacementText() const;
    void setReplacementText(const QString& text);
    int findOptions() const;
    void setFindOptions(int options);
    QFont findPanelFont() const;
    void setFindPanelFont(const QFont& font);
    QColor findPanelBackgroundColor() const;
    void setFindPanelBackgroundColor(const QColor& color);
    QColor findPanelForegroundColor() const;
    void setFindPanelForegroundColor(const QColor& color);
    QColor findPanelFieldBackgroundColor() const;
    void setFindPanelFieldBackgroundColor(const QColor& color);
    QColor findPanelFieldForegroundColor() const;
    void setFindPanelFieldForegroundColor(const QColor& color);
    QColor findPanelBorderColor() const;
    void setFindPanelBorderColor(const QColor& color);
    QColor findPanelButtonHoverColor() const;
    void setFindPanelButtonHoverColor(const QColor& color);
    QColor findPanelDisabledForegroundColor() const;
    void setFindPanelDisabledForegroundColor(const QColor& color);
    QColor findPanelSelectionBackgroundColor() const;
    void setFindPanelSelectionBackgroundColor(const QColor& color);
    QColor findPanelSelectionForegroundColor() const;
    void setFindPanelSelectionForegroundColor(const QColor& color);
    void request_scene_graph_update(
        bool static_content_dirty = false,
        bool needs_style_sync     = false,
        bool scrolling            = false);

    // Replace the text payload that the typed notification signals - `modified`,
    // `uriDropped` and `autoCompleteSelection` - deliver for the notification currently
    // being dispatched. Only meaningful from a slot connected to `notify()`; the bytes
    // are copied here, so this item never reads caller storage after the slot returns.
    // Assigning `Scintilla::NotificationData::text` from such a slot does not change
    // what the typed signals deliver, because this item must not dereference a pointer
    // whose lifetime and extent it does not own. The scalar fields of the notification,
    // including `length`, stay caller-mutable and are forwarded as written.
    void replace_notification_text(const QByteArray& text);

public slots:
    // Scroll events coming from GUI to be sent to Scintilla.
    void scrollHorizontal(int value);
    void scrollVertical(int value);

    // Emit Scintilla notifications as signals.
    void notifyParent(Scintilla::NotificationData scn);

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
    void aboutToCopy(QMimeData* data);

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
        Scintilla::uptr_t  w_param,
        Scintilla::sptr_t  l_param);
    void marginClicked(Scintilla::Position position, Scintilla::KeyMod modifiers, int margin);
    void textAreaClicked(Scintilla::Position line, int modifiers);
    void needShown(Scintilla::Position position, Scintilla::Position length);
    void painted();
    void userListSelection(); // Wants some args.
    void uriDropped(const QString& uri);
    void dwellStart(int x, int y);
    void dwellEnd(int x, int y);
    void zoom(int zoom);
    void hotSpotClick(Scintilla::Position position, Scintilla::KeyMod modifiers);
    void hotSpotDoubleClick(Scintilla::Position position, Scintilla::KeyMod modifiers);
    void callTipClick();
    void autoCompleteSelection(Scintilla::Position position, const QString& text);
    void autoCompleteCancelled();
    void focusChanged(bool focused);

    // Base Scintilla notifications exposed by this item.
    void notify(Scintilla::NotificationData* pscn);
    void command(Scintilla::uptr_t w_param, Scintilla::sptr_t l_param);

    // GUI event notifications needed under Qt
    void buttonPressed(QMouseEvent* event);
    void buttonReleased(QMouseEvent* event);
    void keyPressed(QKeyEvent* event);
    void resized();
    void textChanged();
    void fontChanged();
    void readonlyChanged();
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
    void findPanelVisibleChanged();
    void findReplaceModeChanged();
    void findTextChanged();
    void replacementTextChanged();
    void findOptionsChanged();
    void findPanelFontChanged();
    void findPanelBackgroundColorChanged();
    void findPanelForegroundColorChanged();
    void findPanelFieldBackgroundColorChanged();
    void findPanelFieldForegroundColorChanged();
    void findPanelBorderColorChanged();
    void findPanelButtonHoverColorChanged();
    void findPanelDisabledForegroundColorChanged();
    void findPanelSelectionBackgroundColorChanged();
    void findPanelSelectionForegroundColorChanged();
    void enableScrollViewInteraction(bool value);
    void showContextMenu(const QPoint& pos);
    void addToContextMenu(int menuId, const QString& txt, bool enabled);
    void clearContextMenu();
    void notificationReceived(const ScintillaQuick_notification& notification);

protected:
    bool event(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery property, QVariant argument) const;
    void touchEvent(QTouchEvent* event) override;
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* update_paint_node_data) override;

private:
#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
    friend class Scintilla::Internal::ScintillaQuick_validation_access;
#endif

    class Render_data;
    class Find_panel;
    QString getText() const;
    void setText(const QString& txt);
    QFont getFont() const { return m_font; }
    void setFont(const QFont& newFont);
    void setStylesFont(const QFont& f, int style);
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
    const Scintilla::Internal::Render_frame& rendered_frame_for_test() const;
    void reset_tracked_scroll_width();
    void apply_scene_graph_update_request(
        bool scroll_width_reset,
        bool needed,
        bool static_content_dirty,
        bool needs_style_sync,
        bool scrolling) const;
    sptr_t dispatch_scintilla_message_raw(
        unsigned int i_message,
        uptr_t       w_param,
        sptr_t       l_param) const;
    bool dispatch_direct_edit_message(
        unsigned int i_message,
        uptr_t       w_param,
        sptr_t       l_param,
        sptr_t&      result);
    ScintillaQuick_edit_disposition dispatch_edit(
        ScintillaQuick_edit_replacement replacement);
    ScintillaQuick_edit_disposition dispatch_edits(
        std::vector<ScintillaQuick_edit_replacement> replacements);
    ScintillaQuick_edit_disposition dispatch_edit_span(
        std::span<const ScintillaQuick_edit_replacement> replacements);
    bool single_stream_replacement(
        QByteArray                    inserted_text,
        ScintillaQuick_edit_replacement& replacement,
        bool                          overtype = false) const;
    bool delete_key_replacement(
        bool                           backward,
        ScintillaQuick_edit_replacement& replacement) const;
    bool stream_paste_replacement(
        const QMimeData*                mime_data,
        ScintillaQuick_edit_replacement& replacement) const;
    bool document_edit_message(unsigned int i_message) const;
    std::uint64_t edit_transaction_id();
    void begin_edit_transaction();
    void end_edit_transaction();
    bool edit_handler_active() const;

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

    Scintilla::Internal::ScintillaQuick_core* m_core;

    QElapsedTimer m_elapsed_timer;

    Scintilla::Position m_preedit_pos;
    Scintilla::Position m_handler_ime_anchor = 0;
    Scintilla::Position m_handler_ime_caret  = 0;
    bool m_handler_ime_active = false;
    // Owned text payload of the notification currently being delivered by
    // `notifyParent()`, and the target of `replace_notification_text()`. Points into the
    // active `notifyParent()` frame while `notify()` is being emitted and is null at all
    // other times; saved and restored around that emission so a slot that re-enters
    // Scintilla and triggers a nested notification cannot steal the outer delivery.
    QByteArray* m_delivered_notification_text = nullptr;
    std::unique_ptr<Render_data> m_render_data;
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
    // Declared mutable because `send()` is const for Q_PROPERTY readers, even
    // though mutating Scintilla messages can also flow through it.
    mutable bool m_in_sync_quick_view_properties = false;

    ScintillaQuick_edit_handler m_edit_handler;
    std::uint64_t m_next_edit_transaction_id = 1;
    std::uint64_t m_active_edit_transaction_id = 0;
    int m_edit_transaction_depth = 0;
    int m_evaluating_edit_handler = 0;
    int m_applying_handled_edit = 0;
    ScintillaQuick_edit_disposition m_last_edit_disposition =
        ScintillaQuick_edit_disposition::DECLINED;

    static bool IsHangul(const QChar qchar);
    void MoveImeCarets(Scintilla::Position offset);
    void DrawImeIndicator(int indicator, int len);
    static Scintilla::KeyMod ModifiersOfKeyboard();
    Scintilla::KeyMod ModifiersOfMouse() const;
    void syncQuickViewProperties();
    void ensureFindPanel();
    void updateFindPanelGeometry();

    QPointer<Find_panel> m_find_panel;
    bool m_find_panel_visible = false;
    bool m_find_replace_mode = false;
    QString m_find_text;
    QString m_replacement_text;
    int m_find_options = 0;
    QFont m_find_panel_font;
    QColor m_find_panel_background_color = QColor(45, 45, 48);
    QColor m_find_panel_foreground_color = QColor(230, 230, 230);
    QColor m_find_panel_field_background_color = QColor(37, 37, 38);
    QColor m_find_panel_field_foreground_color = QColor(240, 240, 240);
    QColor m_find_panel_border_color = QColor(63, 63, 70);
    QColor m_find_panel_button_hover_color = QColor(62, 62, 66);
    QColor m_find_panel_disabled_foreground_color = QColor(112, 112, 112);
    QColor m_find_panel_selection_background_color = QColor(38, 79, 120);
    QColor m_find_panel_selection_foreground_color = QColor(255, 255, 255);
    Scintilla::Position m_last_find_start = -1;
    Scintilla::Position m_last_find_end = -1;
    QString m_last_find_text;
    int m_last_find_options = 0;
};

void register_scintilla_type();

#endif
