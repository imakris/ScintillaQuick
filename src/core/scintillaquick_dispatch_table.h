// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// ScintillaQuick dispatch table for scene-graph update requests.
//
// Extracted from ScintillaQuick_item.cpp so that the table can be unit
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

namespace Scintilla::Internal
{

struct scene_graph_update_request_info_t
{
    bool needed               = false;
    bool static_content_dirty = false;
    bool needs_style_sync     = false;
    bool scrolling            = false;
};

// Known read-only / query-like Scintilla messages that are safe to treat
// as non-visual. Messages listed here take the fast path and skip
// scene-graph resync.
//
// CRITICAL invariant: every SCI_* message that ScintillaQuick_item or its
// helper getters call INTERNALLY via `send()` must appear in this list
// if it is a read-only query. If it does not, the conservative default
// in `scene_graph_update_request()` will trigger a full resync, which
// in turn calls `syncQuickViewProperties()` which itself issues SCI_*
// queries through `send()` -- recursing until the stack overflows.
//
// The "internal callers" we must cover today are:
//
//   * ScintillaQuick_item::syncQuickViewProperties()
//   * ScintillaQuick_item::getCharHeight() / getCharWidth()
//   * ScintillaQuick_item::getVisibleLines() / getVisibleColumns()
//   * ScintillaQuick_item::getFirstVisibleColumn() / getFirstVisibleLine()
//   * ScintillaQuick_item::getTotalLines() / getTotalColumns()
//   * ScintillaQuick_item::getText() / getReadonly()
//   * style_attributes_for() (helper in ScintillaQuick_item.cpp)
//   * ProcessScintillaContextMenu() and IME query helpers
//
// Public callers also routinely use `send()` for getter-style queries
// such as SCI_GETLENGTH / SCI_GETMODEVENTMASK / SCI_GETSELECTIONS.
// These must NOT trigger fake content invalidation or full scene-graph
// rebuilds just because they are not on the small internal hot-path
// allow-list; they are semantically queries and do not change what is
// rendered.
inline bool scene_graph_message_is_known_getter(unsigned int i_message)
{
    switch (i_message) {
        case SCI_GETLENGTH:
        case SCI_GETCHARAT:
        case SCI_GETCURRENTPOS:
        case SCI_GETANCHOR:
        case SCI_GETSTYLEAT:
        case SCI_GETSTYLEINDEXAT:
        case SCI_GETSTYLEDTEXT:
        case SCI_GETSTYLEDTEXTFULL:
        case SCI_GETUNDOCOLLECTION:
        case SCI_GETVIEWWS:
        case SCI_GETTABDRAWMODE:
        case SCI_GETCURLINE:
        case SCI_GETENDSTYLED:
        case SCI_GETEOLMODE:
        case SCI_GETBUFFEREDDRAW:
        case SCI_GETTABWIDTH:
        case SCI_GETTABMINIMUMWIDTH:
        case SCI_GETNEXTTABSTOP:
        case SCI_GETFONTLOCALE:
        case SCI_GETIMEINTERACTION:
        case SCI_GETMARGINTYPEN:
        case SCI_GETMARGINWIDTHN:
        case SCI_GETMARGINMASKN:
        case SCI_GETMARGINSENSITIVEN:
        case SCI_GETMARGINCURSORN:
        case SCI_GETMARGINBACKN:
        case SCI_GETMARGINS:
        case SCI_GETELEMENTCOLOUR:
        case SCI_GETELEMENTISSET:
        case SCI_GETELEMENTALLOWSTRANSLUCENT:
        case SCI_GETELEMENTBASECOLOUR:
        case SCI_GETSELALPHA:
        case SCI_GETSELEOLFILLED:
        case SCI_GETSELECTIONLAYER:
        case SCI_GETCARETLINELAYER:
        case SCI_GETCARETLINEHIGHLIGHTSUBLINE:
        case SCI_GETCARETPERIOD:
        case SCI_GETWORDCHARS:
        case SCI_GETCHARACTERCATEGORYOPTIMIZATION:
        case SCI_GETWHITESPACESIZE:
        case SCI_GETLINESTATE:
        case SCI_GETMAXLINESTATE:
        case SCI_GETCARETLINEVISIBLE:
        case SCI_GETCARETLINEBACK:
        case SCI_GETCARETLINEFRAME:
        case SCI_GETINDENT:
        case SCI_GETUSETABS:
        case SCI_GETLINEINDENTATION:
        case SCI_GETLINEINDENTPOSITION:
        case SCI_GETCOLUMN:
        case SCI_GETHSCROLLBAR:
        case SCI_GETINDENTATIONGUIDES:
        case SCI_GETHIGHLIGHTGUIDE:
        case SCI_GETLINEENDPOSITION:
        case SCI_GETCODEPAGE:
        case SCI_GETCARETFORE:
        case SCI_GETREADONLY:
        case SCI_GETSELECTIONSTART:
        case SCI_GETSELECTIONEND:
        case SCI_GETPRINTMAGNIFICATION:
        case SCI_GETPRINTCOLOURMODE:
        case SCI_GETCHANGEHISTORY:
        case SCI_GETFIRSTVISIBLELINE:
        case SCI_GETLINE:
        case SCI_GETLINECOUNT:
        case SCI_GETMARGINLEFT:
        case SCI_GETMARGINRIGHT:
        case SCI_GETMODIFY:
        case SCI_GETSELTEXT:
        case SCI_GETTEXTRANGE:
        case SCI_GETTEXTRANGEFULL:
        case SCI_GETSELECTIONHIDDEN:
        case SCI_GETTEXT:
        case SCI_GETTEXTLENGTH:
        case SCI_GETDIRECTFUNCTION:
        case SCI_GETDIRECTSTATUSFUNCTION:
        case SCI_GETDIRECTPOINTER:
        case SCI_GETOVERTYPE:
        case SCI_GETCARETWIDTH:
        case SCI_GETTARGETSTART:
        case SCI_GETTARGETSTARTVIRTUALSPACE:
        case SCI_GETTARGETEND:
        case SCI_GETTARGETENDVIRTUALSPACE:
        case SCI_GETTARGETTEXT:
        case SCI_GETSEARCHFLAGS:
        case SCI_GETFOLDLEVEL:
        case SCI_GETLASTCHILD:
        case SCI_GETFOLDPARENT:
        case SCI_GETLINEVISIBLE:
        case SCI_GETALLLINESVISIBLE:
        case SCI_GETFOLDEXPANDED:
        case SCI_GETDEFAULTFOLDDISPLAYTEXT:
        case SCI_GETAUTOMATICFOLD:
        case SCI_GETTABINDENTS:
        case SCI_GETBACKSPACEUNINDENTS:
        case SCI_GETMOUSEDWELLTIME:
        case SCI_GETIDLESTYLING:
        case SCI_GETWRAPMODE:
        case SCI_GETWRAPVISUALFLAGS:
        case SCI_GETWRAPVISUALFLAGSLOCATION:
        case SCI_GETWRAPSTARTINDENT:
        case SCI_GETWRAPINDENTMODE:
        case SCI_GETLAYOUTCACHE:
        case SCI_GETSCROLLWIDTH:
        case SCI_GETSCROLLWIDTHTRACKING:
        case SCI_GETENDATLASTLINE:
        case SCI_GETVSCROLLBAR:
        case SCI_GETPHASESDRAW:
        case SCI_GETFONTQUALITY:
        case SCI_GETMULTIPASTE:
        case SCI_GETTAG:
        case SCI_GETACCESSIBILITY:
        case SCI_GETVIEWEOL:
        case SCI_GETDOCPOINTER:
        case SCI_GETEDGECOLUMN:
        case SCI_GETEDGEMODE:
        case SCI_GETEDGECOLOUR:
        case SCI_GETMULTIEDGECOLUMN:
        case SCI_GETZOOM:
        case SCI_GETDOCUMENTOPTIONS:
        case SCI_GETMODEVENTMASK:
        case SCI_GETCOMMANDEVENTS:
        case SCI_GETFOCUS:
        case SCI_GETSTATUS:
        case SCI_GETMOUSEDOWNCAPTURES:
        case SCI_GETMOUSEWHEELCAPTURES:
        case SCI_GETCURSOR:
        case SCI_GETCONTROLCHARSYMBOL:
        case SCI_GETXOFFSET:
        case SCI_GETPRINTWRAPMODE:
        case SCI_GETHOTSPOTACTIVEFORE:
        case SCI_GETHOTSPOTACTIVEBACK:
        case SCI_GETHOTSPOTACTIVEUNDERLINE:
        case SCI_GETHOTSPOTSINGLELINE:
        case SCI_GETSELECTIONMODE:
        case SCI_GETMOVEEXTENDSSELECTION:
        case SCI_GETLINESELSTARTPOSITION:
        case SCI_GETLINESELENDPOSITION:
        case SCI_GETWHITESPACECHARS:
        case SCI_GETPUNCTUATIONCHARS:
        case SCI_GETCARETSTICKY:
        case SCI_GETPASTECONVERTENDINGS:
        case SCI_GETCARETLINEBACKALPHA:
        case SCI_GETCARETSTYLE:
        case SCI_GETINDICATORCURRENT:
        case SCI_GETINDICATORVALUE:
        case SCI_GETPOSITIONCACHE:
        case SCI_GETLAYOUTTHREADS:
        case SCI_GETCHARACTERPOINTER:
        case SCI_GETRANGEPOINTER:
        case SCI_GETGAPPOSITION:
        case SCI_GETEXTRAASCENT:
        case SCI_GETEXTRADESCENT:
        case SCI_GETMARGINOPTIONS:
        case SCI_GETMOUSESELECTIONRECTANGULARSWITCH:
        case SCI_GETMULTIPLESELECTION:
        case SCI_GETADDITIONALSELECTIONTYPING:
        case SCI_GETADDITIONALCARETSBLINK:
        case SCI_GETADDITIONALCARETSVISIBLE:
        case SCI_GETSELECTIONS:
        case SCI_GETSELECTIONEMPTY:
        case SCI_GETMAINSELECTION:
        case SCI_GETSELECTIONNCARET:
        case SCI_GETSELECTIONNANCHOR:
        case SCI_GETSELECTIONNCARETVIRTUALSPACE:
        case SCI_GETSELECTIONNANCHORVIRTUALSPACE:
        case SCI_GETSELECTIONNSTART:
        case SCI_GETSELECTIONNSTARTVIRTUALSPACE:
        case SCI_GETSELECTIONNENDVIRTUALSPACE:
        case SCI_GETSELECTIONNEND:
        case SCI_GETRECTANGULARSELECTIONCARET:
        case SCI_GETRECTANGULARSELECTIONANCHOR:
        case SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE:
        case SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE:
        case SCI_GETVIRTUALSPACEOPTIONS:
        case SCI_GETRECTANGULARSELECTIONMODIFIER:
        case SCI_GETADDITIONALSELALPHA:
        case SCI_GETADDITIONALCARETFORE:
        case SCI_GETIDENTIFIER:
        case SCI_GETTECHNOLOGY:
        case SCI_GETCARETLINEVISIBLEALWAYS:
        case SCI_GETLINEENDTYPESALLOWED:
        case SCI_GETLINEENDTYPESACTIVE:
        case SCI_GETREPRESENTATION:
        case SCI_GETREPRESENTATIONAPPEARANCE:
        case SCI_GETREPRESENTATIONCOLOUR:
        case SCI_GETLINECHARACTERINDEX:
        case SCI_GETLEXER:
        case SCI_GETPROPERTY:
        case SCI_GETPROPERTYEXPANDED:
        case SCI_GETPROPERTYINT:
        case SCI_GETLEXERLANGUAGE:
        case SCI_GETLINEENDTYPESSUPPORTED:
        case SCI_GETSUBSTYLESSTART:
        case SCI_GETSUBSTYLESLENGTH:
        case SCI_GETSTYLEFROMSUBSTYLE:
        case SCI_GETPRIMARYSTYLEFROMSTYLE:
        case SCI_GETSUBSTYLEBASES:
        case SCI_GETNAMEDSTYLES:
        case SCI_GETBIDIRECTIONAL:
            return true;
        default:
            return false;
    }
}

inline bool scene_graph_message_is_known_read_only(unsigned int i_message)
{
    if (scene_graph_message_is_known_getter(i_message)) {
        return true;
    }

    switch (i_message) {
        // Non-GET query helpers with no visual side effects.
        case SCI_CANREDO:
        case SCI_CANPASTE:
        case SCI_CANUNDO:
        case SCI_FINDTEXT:
        case SCI_FINDTEXTFULL:
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
        case SCI_POSITIONFROMPOINTCLOSE:
        case SCI_POINTXFROMPOSITION:
        case SCI_POINTYFROMPOSITION:
        case SCI_WORDSTARTPOSITION:
        case SCI_WORDENDPOSITION:
        case SCI_BRACEMATCH:
        case SCI_BRACEMATCHNEXT:
        case SCI_POSITIONRELATIVE:
        case SCI_POSITIONRELATIVECODEUNITS:
        case SCI_CHARPOSITIONFROMPOINT:
        case SCI_CHARPOSITIONFROMPOINTCLOSE:
        case SCI_COUNTCHARACTERS:
        case SCI_COUNTCODEUNITS:
        // Line / position lookups. These are pure read-only queries but
        // are not named SCI_GET*, so they fell through to the conservative
        // default branch and silently marked the scene graph dirty. That
        // defeated the vertical-scroll reuse fast path in
        // `build_render_snapshot()` whenever an application (or a
        // benchmark verifier) inspected line / position state between
        // scroll steps: each query set `content_modified_since_last_capture`
        // and forced the next frame to take a full recapture instead of
        // the buffer-translate fast path. Classifying them correctly as
        // read-only is both semantically accurate and unlocks the
        // scroll-reuse path they were inadvertently disabling.
        case SCI_POSITIONFROMLINE:
        case SCI_LINELENGTH:
        case SCI_VISIBLEFROMDOCLINE:
        case SCI_DOCLINEFROMVISIBLE:
        case SCI_WRAPCOUNT:
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

inline scene_graph_update_request_info_t scene_graph_update_request(unsigned int i_message)
{
    switch (i_message) {
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
        // Caret / selection movement commands. These are mutators on the
        // caret and selection state, so they DO need a scene-graph
        // update request, but they do NOT modify document text or
        // styles. Classifying them here (rather than letting them fall
        // through to the conservative full-resync default) is a
        // double win:
        //
        //   1. `static_content_dirty` stays false, so
        //      `build_render_snapshot()` takes the overlay-only capture
        //      path and skips the `capture_current_frame.paint_text`
        //      work for every visual line.
        //   2. `needs_style_sync` stays false, so `send()` skips the
        //      `syncQuickViewProperties()` call that re-queries all
        //      the exported editor properties and emits any number of
        //      property-change signals per caret move.
        //
        // The tight `caret_move_right_5000` benchmark loop issues
        // SCI_CHARRIGHT 5000 times. Without the explicit
        // classification below, each call pays the full resync cost.
        // The editor still repaints the caret
        // and selection exactly as before, but the per-call cost of
        // each movement collapses from "full resync" to "overlay-only
        // resync".
        //
        // If a caret movement happens to scroll the view (e.g.
        // CHARRIGHT at end of a long wrapped line jumps to the next
        // visible line), Scintilla will fire `Notification::UpdateUI`
        // with the `Update::VScroll` bit set, and
        // `updateQuickView()` in `notifyParent` will observe the
        // scroll and follow up with the appropriate scroll-aware
        // `request_scene_graph_update`. So scroll handling is not
        // lost; it just moves from the dispatch table to the Update
        // notification path, which is where Scintilla actually knows
        // whether a scroll happened.
        case SCI_LINEDOWN:
        case SCI_LINEDOWNEXTEND:
        case SCI_LINEDOWNRECTEXTEND:
        case SCI_LINEUP:
        case SCI_LINEUPEXTEND:
        case SCI_LINEUPRECTEXTEND:
        case SCI_CHARLEFT:
        case SCI_CHARLEFTEXTEND:
        case SCI_CHARLEFTRECTEXTEND:
        case SCI_CHARRIGHT:
        case SCI_CHARRIGHTEXTEND:
        case SCI_CHARRIGHTRECTEXTEND:
        case SCI_WORDLEFT:
        case SCI_WORDLEFTEXTEND:
        case SCI_WORDRIGHT:
        case SCI_WORDRIGHTEXTEND:
        case SCI_WORDLEFTEND:
        case SCI_WORDLEFTENDEXTEND:
        case SCI_WORDRIGHTEND:
        case SCI_WORDRIGHTENDEXTEND:
        case SCI_WORDPARTLEFT:
        case SCI_WORDPARTLEFTEXTEND:
        case SCI_WORDPARTRIGHT:
        case SCI_WORDPARTRIGHTEXTEND:
        case SCI_HOME:
        case SCI_HOMEEXTEND:
        case SCI_HOMERECTEXTEND:
        case SCI_HOMEDISPLAY:
        case SCI_HOMEDISPLAYEXTEND:
        case SCI_HOMEWRAP:
        case SCI_HOMEWRAPEXTEND:
        case SCI_VCHOME:
        case SCI_VCHOMEEXTEND:
        case SCI_VCHOMERECTEXTEND:
        case SCI_VCHOMEDISPLAY:
        case SCI_VCHOMEDISPLAYEXTEND:
        case SCI_VCHOMEWRAP:
        case SCI_VCHOMEWRAPEXTEND:
        case SCI_LINEEND:
        case SCI_LINEENDEXTEND:
        case SCI_LINEENDRECTEXTEND:
        case SCI_LINEENDDISPLAY:
        case SCI_LINEENDDISPLAYEXTEND:
        case SCI_LINEENDWRAP:
        case SCI_LINEENDWRAPEXTEND:
        case SCI_DOCUMENTSTART:
        case SCI_DOCUMENTSTARTEXTEND:
        case SCI_DOCUMENTEND:
        case SCI_DOCUMENTENDEXTEND:
        case SCI_PAGEUP:
        case SCI_PAGEUPEXTEND:
        case SCI_PAGEUPRECTEXTEND:
        case SCI_PAGEDOWN:
        case SCI_PAGEDOWNEXTEND:
        case SCI_PAGEDOWNRECTEXTEND:
        case SCI_STUTTEREDPAGEUP:
        case SCI_STUTTEREDPAGEUPEXTEND:
        case SCI_STUTTEREDPAGEDOWN:
        case SCI_STUTTEREDPAGEDOWNEXTEND:
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
    if (scene_graph_message_is_known_read_only(i_message)) {
        return {};
    }

    // Unknown message: default to a conservative full resync. Repainting
    // too often is a performance bug; repainting too rarely is a visual
    // correctness bug, and new Scintilla messages that mutate state would
    // otherwise be silently skipped here.
    return {true, true, true, false};
}

inline bool tracked_scroll_width_should_reset(unsigned int i_message)
{
    switch (i_message) {
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
