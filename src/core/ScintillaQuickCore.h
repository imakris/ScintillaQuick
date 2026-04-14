// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.
//
// @file ScintillaQuickCore.h - Qt specific subclass of ScintillaBase

#ifndef SCINTILLAQUICKCORE_H
#define SCINTILLAQUICKCORE_H

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
#include <QClipboard>  // QClipboard::Mode enum is used by-value below.

// All of these only appear as pointer parameters, pointer return types,
// or override parameters in this header, so forward declarations are
// sufficient. The full headers are included from the .cpp files that
// actually need them.
class QAction;
class QMimeData;
class QPainter;
class QQuickItem;
class QTimerEvent;

class ScintillaQuickItem;

namespace Scintilla::Internal {

class ScintillaQuickCore : public QObject, public ScintillaBase
{
    Q_OBJECT

public:
    explicit ScintillaQuickCore(::ScintillaQuickItem *parent);
    void UpdateInfos(int winId);
    void ensure_visible_range_styled(bool scrolling);
    void selectCurrentWord();
    void reset_tracked_scroll_width_to_viewport();
    render_frame current_render_frame(
        const captured_frame *capture_frame = nullptr,
        bool static_content_dirty           = true,
        bool ensure_styled                  = true,
        bool scrolling                      = false,
        int  extra_capture_lines            = 0);

    // Called from `~ScintillaQuickItem()` before the derived
    // `ScintillaQuickItem` subobject finishes destructing. Stops all
    // timers (which may otherwise fire via Qt's event loop between the
    // derived destructor body and Qt's child-deletion pass) and nulls
    // the back-pointer so that any still-queued slot invocations that
    // reach `ScintillaQuickCore` after this point are inert. Without
    // this step, a queued `timerEvent` or clipboard `SelectionChanged`
    // can race against the owning `ScintillaQuickItem` destructor and
    // reach a sliced-down QQuickItem.
    void prepare_for_owner_destruction();

    virtual ~ScintillaQuickCore();

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
    void aboutToCopy(QMimeData *data);

    void command(Scintilla::uptr_t wParam, Scintilla::sptr_t lParam);

private slots:
    void onIdle();
    void execCommand(QAction *action);
    void SelectionChanged();

private:
    void Init();
    void Finalise() override;
    bool DragThreshold(Point ptStart, Point ptNow) override;
    bool ValidCodePage(int codePage) const override;
    std::string UTF8FromEncoded(std::string_view encoded) const override;
    std::string EncodedFromUTF8(std::string_view utf8) const override;

private:
#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
    friend class Scintilla::Internal::scintillaquick_validation_access;
#endif

    void SetVerticalScrollPos() override;
    void SetHorizontalScrollPos() override;
    bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override;
    void CopyToModeClipboard(const SelectionText &selectedText, QClipboard::Mode clipboardMode_);
    void Copy() override;
    void CopyToClipboard(const SelectionText &selectedText) override;
    void PasteFromMode(QClipboard::Mode clipboardMode_);
    void Paste() override;
    void ClaimSelection() override;
    void NotifyChange() override;
    void NotifyFocus(bool focus) override;
    void NotifyParent(Scintilla::NotificationData scn) override;
    void NotifyURIDropped(const char *uri);
    int  timers[static_cast<size_t>(TickReason::dwell)+1]{};
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
    QString StringFromDocument(const char *s) const;
    QByteArray BytesForDocument(const QString &text) const;
    std::unique_ptr<CaseFolder> CaseFolderForEncoding() override;
    std::string CaseMapString(const std::string &s, CaseMapping caseMapping) override;

    void CreateCallTipWindow(PRectangle rc) override;
    void AddToPopUp(const char *label, int cmd, bool enabled) override;
public:
    sptr_t WndProc(Scintilla::Message iMessage, uptr_t wParam, sptr_t lParam) override;
    sptr_t DefWndProc(Scintilla::Message iMessage, uptr_t wParam, sptr_t lParam) override;
private:
    static sptr_t DirectFunction(
        sptr_t ptr, unsigned int iMessage, uptr_t wParam, sptr_t lParam);
    static sptr_t DirectStatusFunction(
        sptr_t ptr, unsigned int iMessage, uptr_t wParam, sptr_t lParam, int *pStatus);
    struct style_attributes;
    captured_frame capture_current_frame(
        bool static_content_dirty, bool ensure_styled, bool scrolling, int extra_capture_lines = 0);
    render_frame render_frame_from_capture(const captured_frame &capture_frame) const;
    style_attributes style_attributes_for(int style) const;

    QPainter *GetPainter() { return m_current_painter; }

protected:

    void PartialPaint(const PRectangle &rect);
    void PartialPaintQml(const PRectangle & rect, QPainter *painter);

    void DragEnter(const Point &point);
    void DragMove(const Point &point);
    void DragLeave();
    void Drop(const Point &point, const QMimeData *data, bool move);
    void DropUrls(const QMimeData *data);

    void timerEvent(QTimerEvent *event) override;

private:
    // Non-owning back-pointer to the enclosing QQuickItem. Historically
    // named `scrollArea` because upstream Scintilla's Qt integration
    // embedded the editor in a QScrollArea-shaped widget; in the Qt
    // Quick build the "scroll area" and the editor item are the same
    // object. Parenting is handled by the QObject parent link set up
    // in the constructor.
    ::ScintillaQuickItem *m_owner;

    // Owning idle timer. Previously this was a bare `new QTimer`
    // stashed through `idler.idlerID` as `void*`, which leaked if
    // `SetIdle(false)` was never called before destruction and could
    // fire with a dangling receiver. The unique_ptr is parented to
    // `this` so Qt's object tree handles cleanup as a fallback even
    // if `ChangeIdle(false)` is somehow missed.
    std::unique_ptr<QTimer> m_idle_timer;

    int  m_v_max,  m_h_max;   // Scroll bar maximums.
    int  m_v_page, m_h_page; // Scroll bar page sizes.

    bool m_have_mouse_capture;
    bool m_drag_was_dropped;
    int  m_rectangular_selection_modifier;

    QPainter *m_current_painter;  // temporary variable for paint() handling

    friend class ::ScintillaQuickItem;
};

}

#endif /* SCINTILLAQUICKCORE_H */

