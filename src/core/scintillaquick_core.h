// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file scintillaquick_core.h - Qt-specific ScintillaBase subclass.

#ifndef SCINTILLAQUICK_CORE_H
#define SCINTILLAQUICK_CORE_H

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <memory>

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"
#include "Scintilla.h"
#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "ILoader.h"
#include "ILexer.h"
#include "Position.h"
#include "UniqueString.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "LineMarker.h"
#include "Style.h"
#include "AutoComplete.h"
#include "ViewStyle.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "CaseFolder.h"
#include "CharacterCategoryMap.h"
#include "Document.h"
#include "Selection.h"
#include "PositionCache.h"
#include "EditModel.h"
#include "MarginView.h"
#include "EditView.h"
#include "Editor.h"
#include "ScintillaBase.h"
#include "CaseConvert.h"
#include "render_frame.h"

#include <QObject>
#include <QClipboard> // QClipboard::Mode enum is used by-value below.

// All of these only appear as pointer parameters, pointer return types,
// or override parameters in this header, so forward declarations are
// sufficient. The full headers are included from the .cpp files that
// actually need them.
class QAction;
class QMimeData;
class QPainter;
class QQuickItem;
class QTimerEvent;

class ScintillaQuick_item;

#ifndef SCINTILLAQUICK_EXPORT
#if defined(SCINTILLAQUICK_STATIC_DEFINE)
#define SCINTILLAQUICK_EXPORT
#elif defined(WIN32)
#ifdef MAKING_LIBRARY
#define SCINTILLAQUICK_EXPORT __declspec(dllexport)
#else
#define SCINTILLAQUICK_EXPORT __declspec(dllimport)
#endif
#else
#define SCINTILLAQUICK_EXPORT
#endif
#endif

namespace Scintilla::Internal
{

class SCINTILLAQUICK_EXPORT ScintillaQuick_core : public QObject, public ScintillaBase
{
    Q_OBJECT

public:
    explicit ScintillaQuick_core(::ScintillaQuick_item* parent);
    void UpdateInfos(int winId);
    void ensure_visible_range_styled(bool scrolling);
    void selectCurrentWord();
    void reset_tracked_scroll_width_to_viewport();
    Render_frame current_render_frame(
        const Captured_frame* capture_frame = nullptr,
        bool static_content_dirty           = true,
        bool ensure_styled                  = true,
        bool scrolling                      = false,
        int extra_capture_lines             = 0);

    // Called from `~ScintillaQuick_item()` before the derived
    // `ScintillaQuick_item` subobject finishes destructing. Stops all
    // timers (which may otherwise fire via Qt's event loop between the
    // derived destructor body and Qt's child-deletion pass) and nulls
    // the back-pointer so that any still-queued slot invocations that
    // reach `ScintillaQuick_core` after this point are inert. Without
    // this step, a queued `timerEvent` or clipboard `SelectionChanged`
    // can race against the owning `ScintillaQuick_item` destructor and
    // reach a sliced-down QQuickItem.
    void prepare_for_owner_destruction();

    virtual ~ScintillaQuick_core();

signals:
    void cursorPositionChanged();
    void horizontalScrolled(int value);
    void verticalScrolled(int value);
    void horizontalRangeChanged(int max, int page);
    void verticalRangeChanged(int max, int page);

    void notifyParent(Scintilla::NotificationData scn);
    void notifyChange();

    // Clients can use this hook to add additional
    // formats (e.g. rich text) to the MIME data.
    void aboutToCopy(QMimeData* data);

    void command(Scintilla::uptr_t w_param, Scintilla::sptr_t l_param);

private slots:
    void onIdle();
    void execCommand(QAction* action);
    void SelectionChanged();

private:
    void Init();
    void Finalise() override;
    bool DragThreshold(Point pt_start, Point pt_now) override;
    bool ValidCodePage(int code_page) const override;
    std::string UTF8FromEncoded(std::string_view encoded) const override;
    std::string EncodedFromUTF8(std::string_view utf8) const override;

private:
#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
    friend class Scintilla::Internal::ScintillaQuick_validation_access;
#endif

    void SetVerticalScrollPos() override;
    void SetHorizontalScrollPos() override;
    bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override;
    void CopyToModeClipboard(const SelectionText& selected_text, QClipboard::Mode clipboard_mode);
    void Copy() override;
    void CopyToClipboard(const SelectionText& selected_text) override;
    void PasteFromMode(QClipboard::Mode clipboard_mode);
    void Paste() override;
    void ClaimSelection() override;
    void NotifyChange() override;
    void NotifyFocus(bool focus) override;
    void NotifyParent(Scintilla::NotificationData scn) override;
    void NotifyURIDropped(const char* uri);
    int timers[static_cast<size_t>(TickReason::dwell) + 1]{};
    bool FineTickerRunning(TickReason reason) override;
    void FineTickerStart(TickReason reason, int millis, int tolerance) override;
    void CancelTimers();
    void FineTickerCancel(TickReason reason) override;
    bool ChangeIdle(bool on);
    bool SetIdle(bool on) override;
    void SetMouseCapture(bool on) override;
    bool HaveMouseCapture() override;
    void StartDrag() override;
    Scintilla::CharacterSet CharacterSetOfDocument() const;
    QString StringFromDocument(const char* s) const;
    QByteArray BytesForDocument(const QString& text) const;
    std::unique_ptr<CaseFolder> CaseFolderForEncoding() override;
    std::string CaseMapString(const std::string& s, CaseMapping caseMapping) override;

    void CreateCallTipWindow(PRectangle rc) override;
    void AddToPopUp(const char* label, int cmd, bool enabled) override;

public:
    sptr_t WndProc(Scintilla::Message i_message, uptr_t w_param, sptr_t l_param) override;
    sptr_t DefWndProc(Scintilla::Message i_message, uptr_t w_param, sptr_t l_param) override;

private:
    static sptr_t DirectFunction(sptr_t ptr, unsigned int i_message, uptr_t w_param, sptr_t l_param);
    static sptr_t DirectStatusFunction(
        sptr_t       ptr,
        unsigned int i_message,
        uptr_t       w_param,
        sptr_t       l_param,
        int*         p_status);
    struct Style_attributes;
    Captured_frame capture_current_frame(
        bool static_content_dirty,
        bool ensure_styled,
        bool scrolling,
        int  extra_capture_lines = 0);
    Render_frame render_frame_from_capture(const Captured_frame& capture_frame) const;
    Style_attributes style_attributes_for(int style) const;

    QPainter* GetPainter() { return m_current_painter; }

protected:
    void PartialPaint(const PRectangle& rect);
    void PartialPaintQml(const PRectangle& rect, QPainter* painter);

    void DragEnter(const Point& point);
    void DragMove( const Point& point);
    void DragLeave();
    void Drop(const Point& point, const QMimeData* data, bool move);
    void DropUrls(const QMimeData* data);

    void timerEvent(QTimerEvent* event) override;

private:
    // Non-owning back-pointer to the enclosing editor item. Parenting is
    // handled by the QObject parent link established in the constructor.
    ::ScintillaQuick_item* m_owner;

    // Owning idle timer. The unique_ptr makes ownership explicit while the
    // QObject parent link provides cleanup as a fallback.
    std::unique_ptr<QTimer> m_idle_timer;

    int m_v_max,  m_h_max;   // Scroll bar maximums.
    int m_v_page, m_h_page;  // Scroll bar page sizes.

    bool m_have_mouse_capture;
    bool m_drag_was_dropped;
    int  m_rectangular_selection_modifier;

    QPainter* m_current_painter; // temporary variable for paint() handling

    friend class ::ScintillaQuick_item;
};

} // namespace Scintilla::Internal

#endif
