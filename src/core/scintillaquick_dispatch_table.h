// ScintillaQuick dispatch table for scene-graph update requests.
//
// Extracted from ScintillaQuickItem.cpp so the table can be unit tested
// without spinning up a Qt Quick window.
//
// ROLE OF THIS FILE
// -----------------
//
// This is a FAST-PATH PRE-SCHEDULER, not a correctness mechanism. For
// messages we statically know are mutators, the entries below let
// `send()` schedule a scene-graph update synchronously, without waiting
// for Scintilla's own `NotifyParent()` round-trip.
//
// The AUTHORITATIVE mutation-to-scene-graph link for every other
// message is Scintilla's own notification path:
//
//     ScintillaQuickCore::NotifyParent()           (src/core)
//         -> emit notifyParent(scn)
//             -> ScintillaQuickItem::notifyParent() (src/public)
//                 -> case Notification::Modified / StyleNeeded / UpdateUI:
//                      m_render_data->content_modified_since_last_capture = true;
//                      request_scene_graph_update(...);
//
// That slot is already set up in the item constructor and runs for
// every Scintilla mutation regardless of whether the message that
// caused the mutation was in the allow-list below. Any message not
// listed here therefore falls through to `return {}` (no dispatch
// scheduling) and relies on the notification path.
//
// This is IMPORTANT for the public `send()` API: external callers may
// issue common read-only queries that are NOT listed below
// (SCI_GETLENGTH, SCI_GETSELECTIONS, SCI_GETMODEVENTMASK, ...). The
// default MUST be a no-op so those queries don't spuriously mark
// content dirty and force repaint work -- that was the regression
// Codex flagged on d7f1975.
//
// Consequences for adding new messages
// ------------------------------------
//
// * If a new message MUTATES state and is called frequently enough to
//   matter for scheduling latency, add it to
//   `scene_graph_update_request()` with the appropriate fields. If you
//   forget, correctness is still preserved by the notification path;
//   you just pay one extra callback hop.
//
// * If a new message is READ-ONLY, you do not need to do anything.
//   The default `return {}` is the correct answer.
//
// * If an internal library helper issues a new `send(SCI_FOO)` from
//   inside `syncQuickViewProperties()` or a function it calls, it
//   MUST either be read-only (default path) or your newly-added
//   mutator entry must NOT set `needed=true` for that message.
//   Otherwise you will re-enter the dispatch and recurse; the
//   re-entry guard in ScintillaQuickItem::send() catches this and
//   the regression test
//   `test_sync_quick_view_properties_path_is_recursion_safe` in
//   tests/dispatch_table/main.cpp will fail with a clear message.

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
            // Fall through to the notification-path default. Scintilla's
            // NotifyParent() will call back with Notification::Modified /
            // StyleNeeded / UpdateUI for any message that actually mutates
            // state, and ScintillaQuickItem::notifyParent() will mark the
            // render data dirty and schedule the scene-graph update from
            // that callback.
            //
            // For read-only queries this is the correct outcome too: no
            // dispatch work, no repaint scheduling, no spurious marking
            // of content as dirty. This is what keeps
            // `editor.send(SCI_GETLENGTH)` and similar public-API calls
            // cheap.
            return {};
    }
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
