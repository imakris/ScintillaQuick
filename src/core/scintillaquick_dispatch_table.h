// ScintillaQuick dispatch table for scene-graph update requests.
//
// Extracted from ScintillaQuickItem.cpp so that the table can be unit
// tested directly without spinning up a Qt Quick window.
//
// The key correctness property the tests guard is:
//
//   For every Scintilla message that is not explicitly known to be a
//   mutator and not explicitly known to be a pure read-only query, the
//   dispatch function must fall back to a CONSERVATIVE FULL RESYNC.
//
// Historically the default branch returned "no update needed", which
// meant any Scintilla message not in the allow-list silently skipped a
// required scene-graph resync.

#ifndef SCINTILLAQUICK_DISPATCH_TABLE_H
#define SCINTILLAQUICK_DISPATCH_TABLE_H

#include "Scintilla.h"

namespace Scintilla::Internal {

struct scene_graph_update_request_t
{
    bool needed = false;
    bool static_content_dirty = false;
    bool needs_style_sync = false;
    bool scrolling = false;
};

// Known read-only / pure-query Scintilla messages that the library itself
// issues on the hot path. Messages listed here take the fast path and
// skip scene-graph resync.
//
// CRITICAL invariant: every SCI_* message that ScintillaQuickItem or its
// helper getters call INTERNALLY via `send()` must appear in this list
// if it is a read-only query. If it does not, the conservative default
// in `scene_graph_update_request()` will trigger a full resync, which
// in turn calls `syncQuickViewProperties()` which itself issues SCI_*
// queries through `send()` -- recursing until the stack overflows.
//
// The "internal callers" we must cover today are:
//
//   * ScintillaQuickItem::syncQuickViewProperties()
//   * ScintillaQuickItem::getCharHeight() / getCharWidth()
//   * ScintillaQuickItem::getVisibleLines() / getVisibleColumns()
//   * ScintillaQuickItem::getFirstVisibleColumn() / getFirstVisibleLine()
//   * ScintillaQuickItem::getTotalLines() / getTotalColumns()
//   * ScintillaQuickItem::getText() / getReadonly()
//   * style_attributes_for() (helper in ScintillaQuickItem.cpp)
//   * ProcessScintillaContextMenu() and IME query helpers
//
// External callers issuing *other* SCI_GET* messages will pay the cost
// of a conservative full-sync (which is safe because `send()` guards
// against recursion below the dispatch, see `syncQuickViewProperties`'s
// re-entry guard).
inline bool scene_graph_message_is_known_read_only(unsigned int iMessage)
{
    switch (iMessage) {
        // Core document queries.
        case SCI_GETTEXT:
        case SCI_GETTEXTLENGTH:
        case SCI_GETTEXTRANGE:
        case SCI_GETSELTEXT:
        case SCI_GETCURRENTPOS:
        case SCI_GETANCHOR:
        case SCI_GETFIRSTVISIBLELINE:
        case SCI_GETLINECOUNT:
        case SCI_GETXOFFSET:
        case SCI_GETSCROLLWIDTH:
        case SCI_GETSTYLEAT:
        case SCI_GETCARETWIDTH:
        case SCI_GETCARETPERIOD:
        case SCI_GETZOOM:
        case SCI_GETREADONLY:
        case SCI_GETMARGINWIDTHN:
        case SCI_GETMARGINTYPEN:
        case SCI_GETMARGINMASKN:
        case SCI_GETMARGINBACKN:
        // Geometry / layout queries called from getCharHeight(),
        // getCharWidth(), getVisibleLines() and the mouse-event / IME
        // helpers. Omitting any of these produced a stack-overflow
        // recursion when the conservative resync default was added,
        // because `syncQuickViewProperties()` re-enters `send()` via
        // these same calls.
        case SCI_TEXTHEIGHT:
        case SCI_TEXTWIDTH:
        case SCI_LINESONSCREEN:
        case SCI_LINEFROMPOSITION:
        case SCI_POSITIONFROMPOINT:
        // Style queries called from style_attributes_for() so that
        // per-style font / colour sync during
        // `syncQuickViewProperties()` does not recurse.
        case SCI_STYLEGETFORE:
        case SCI_STYLEGETBACK:
        case SCI_STYLEGETSIZE:
        case SCI_STYLEGETSIZEFRACTIONAL:
        case SCI_STYLEGETWEIGHT:
        case SCI_STYLEGETITALIC:
        case SCI_STYLEGETUNDERLINE:
            return true;
        default:
            return false;
    }
}

inline scene_graph_update_request_t scene_graph_update_request(unsigned int iMessage)
{
    switch (iMessage) {
        case SCI_SETXOFFSET:
            return {true, true, false, false};

        case SCI_SETDOCPOINTER:
            return {true, true, true, false};

        case SCI_SETFIRSTVISIBLELINE:
            return {true, true, true, true};

        case SCI_SETSEL:
        case SCI_SETEMPTYSELECTION:
        case SCI_GOTOPOS:
        case SCI_SETCURRENTPOS:
        case SCI_SETANCHOR:
            return {true, false, false, false};

        case SCI_SETSELFORE:
        case SCI_SETSELBACK:
        case SCI_SETCARETFORE:
        case SCI_SETCARETWIDTH:
        case SCI_SETCARETLINEVISIBLE:
        case SCI_SETCARETLINEBACK:
            return {true, false, false, false};

        case SCI_SETTEXT:
        case SCI_CLEARALL:
        case SCI_INSERTTEXT:
        case SCI_APPENDTEXT:
        case SCI_REPLACESEL:
        case SCI_DELETERANGE:
        case SCI_STYLECLEARALL:
        case SCI_STYLESETFORE:
        case SCI_STYLESETBACK:
        case SCI_STYLESETFONT:
        case SCI_STYLESETSIZE:
        case SCI_STYLESETSIZEFRACTIONAL:
        case SCI_STYLESETBOLD:
        case SCI_STYLESETWEIGHT:
        case SCI_STYLESETITALIC:
        case SCI_STYLESETUNDERLINE:
        case SCI_STYLESETVISIBLE:
        case SCI_STYLESETEOLFILLED:
        case SCI_STYLESETCHARACTERSET:
        case SCI_SETTABWIDTH:
        case SCI_SETUSETABS:
        case SCI_SETWRAPMODE:
        case SCI_SETBIDIRECTIONAL:
        case SCI_SETSCROLLWIDTH:
        case SCI_SETMARGINWIDTHN:
        case SCI_SETMARGINTYPEN:
        case SCI_SETMARGINMASKN:
        case SCI_SETMARGINSENSITIVEN:
        case SCI_SETMARGINLEFT:
        case SCI_SETMARGINRIGHT:
        case SCI_MARKERDEFINE:
        case SCI_MARKERSETFORE:
        case SCI_MARKERSETBACK:
        case SCI_MARKERSETBACKSELECTED:
        case SCI_MARKERSETFORETRANSLUCENT:
        case SCI_MARKERSETBACKTRANSLUCENT:
        case SCI_MARKERSETBACKSELECTEDTRANSLUCENT:
        case SCI_MARKERENABLEHIGHLIGHT:
        case SCI_MARKERADD:
        case SCI_MARKERADDSET:
        case SCI_MARKERDELETE:
        case SCI_MARKERDELETEALL:
        case SCI_MARKERDELETEHANDLE:
        case SCI_MARKERDEFINEPIXMAP:
        case SCI_MARKERDEFINERGBAIMAGE:
        case SCI_SETREADONLY:
            return {true, true, true, false};
        default:
            break;
    }

    // Fast path for the read-only messages the library itself calls frequently.
    if (scene_graph_message_is_known_read_only(iMessage)) {
        return {};
    }

    // Unknown message: default to a conservative full resync. Repainting
    // too often is a performance bug; repainting too rarely is a visual
    // correctness bug, and new Scintilla messages that mutate state would
    // otherwise be silently skipped here.
    return {true, true, true, false};
}

inline bool tracked_scroll_width_should_reset(unsigned int iMessage)
{
    switch (iMessage) {
        case SCI_SETTEXT:
        case SCI_CLEARALL:
        case SCI_STYLECLEARALL:
        case SCI_STYLESETFONT:
        case SCI_STYLESETSIZE:
        case SCI_STYLESETSIZEFRACTIONAL:
        case SCI_STYLESETBOLD:
        case SCI_STYLESETWEIGHT:
        case SCI_STYLESETITALIC:
        case SCI_STYLESETCHARACTERSET:
        case SCI_SETTABWIDTH:
        case SCI_SETUSETABS:
        case SCI_SETWRAPMODE:
        case SCI_SETMARGINWIDTHN:
        case SCI_SETMARGINLEFT:
        case SCI_SETMARGINRIGHT:
        case SCI_SETDOCPOINTER:
            return true;
        default:
            return false;
    }
}

} // namespace Scintilla::Internal

#endif // SCINTILLAQUICK_DISPATCH_TABLE_H
