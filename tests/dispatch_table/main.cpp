// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

// Unit tests for the scene-graph update dispatch table
// (src/core/scintillaquick_dispatch_table.h).
//
// These tests deliberately do NOT spin up a Qt Quick window; the dispatch
// table is a pure function and we assert against it directly. The key
// correctness property being protected here is:
//
//     Unknown Scintilla messages MUST fall through to a conservative
//     full resync (`needed=true`, `static_content_dirty=true`,
//     `needs_style_sync=true`).
//
// Historically the `default:` branch returned `{}` (no update), meaning
// any Scintilla message that happened not to be in the allow-list was
// silently skipped for scene-graph resync. New mutating messages added
// upstream by Scintilla would then leave the editor rendering stale
// content until the next unrelated update.

#include "scintillaquick_dispatch_table.h"
#include "scintillaquick_test_macros.h"

#include "Scintilla.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{

int g_failures = 0;

using Scintilla::Internal::scene_graph_message_is_known_read_only;
using Scintilla::Internal::scene_graph_message_is_known_getter;
using Scintilla::Internal::scene_graph_update_request;
using Scintilla::Internal::scene_graph_update_request_info_t;
using Scintilla::Internal::tracked_scroll_width_should_reset;

struct Legacy_dispatch_rule
{
    unsigned int message;
    bool known_getter;
    bool known_read_only;
    bool needed;
    bool static_content_dirty;
    bool needs_style_sync;
    bool scrolling;
    bool scroll_width_reset;
};

// Independent Stage 1 equivalence oracle captured from the pre-table
// switch classifier. Unknown messages intentionally stay absent from
// this table and use the legacy conservative full-resync fallback in
// legacy_dispatch_for_message().
const Legacy_dispatch_rule k_legacy_dispatch_rules[] = {
    {SCI_INSERTTEXT, false, false, true, true, true, false, false},
    {SCI_CLEARALL, false, false, true, true, true, false, true},
    {SCI_GETLENGTH, true, true, false, false, false, false, false},
    {SCI_GETCHARAT, true, true, false, false, false, false, false},
    {SCI_GETCURRENTPOS, true, true, false, false, false, false, false},
    {SCI_GETANCHOR, true, true, false, false, false, false, false},
    {SCI_GETSTYLEAT, true, true, false, false, false, false, false},
    {SCI_GETSTYLEDTEXT, true, true, false, false, false, false, false},
    {SCI_CANREDO, false, true, false, false, false, false, false},
    {SCI_MARKERLINEFROMHANDLE, false, true, false, false, false, false, false},
    {SCI_MARKERDELETEHANDLE, false, false, true, true, true, false, false},
    {SCI_GETUNDOCOLLECTION, true, true, false, false, false, false, false},
    {SCI_GETVIEWWS, true, true, false, false, false, false, false},
    {SCI_POSITIONFROMPOINT, false, true, false, false, false, false, false},
    {SCI_POSITIONFROMPOINTCLOSE, false, true, false, false, false, false, false},
    {SCI_GOTOPOS, false, false, true, false, false, false, false},
    {SCI_SETANCHOR, false, false, true, false, false, false, false},
    {SCI_GETCURLINE, true, true, false, false, false, false, false},
    {SCI_GETENDSTYLED, true, true, false, false, false, false, false},
    {SCI_GETEOLMODE, true, true, false, false, false, false, false},
    {SCI_GETBUFFEREDDRAW, true, true, false, false, false, false, false},
    {SCI_SETTABWIDTH, false, false, true, true, true, false, true},
    {SCI_GETSTYLEINDEXAT, true, true, false, false, false, false, false},
    {SCI_GETTEXTRANGEFULL, true, true, false, false, false, false, false},
    {SCI_MARKERDEFINE, false, false, true, true, true, false, false},
    {SCI_MARKERSETFORE, false, false, true, true, true, false, false},
    {SCI_MARKERSETBACK, false, false, true, true, true, false, false},
    {SCI_MARKERADD, false, false, true, true, true, false, false},
    {SCI_MARKERDELETE, false, false, true, true, true, false, false},
    {SCI_MARKERDELETEALL, false, false, true, true, true, false, false},
    {SCI_MARKERGET, false, true, false, false, false, false, false},
    {SCI_MARKERNEXT, false, true, false, false, false, false, false},
    {SCI_MARKERPREVIOUS, false, true, false, false, false, false, false},
    {SCI_MARKERDEFINEPIXMAP, false, false, true, true, true, false, false},
    {SCI_STYLECLEARALL, false, false, true, true, true, false, true},
    {SCI_STYLESETFORE, false, false, true, true, true, false, false},
    {SCI_STYLESETBACK, false, false, true, true, true, false, false},
    {SCI_STYLESETBOLD, false, false, true, true, true, false, true},
    {SCI_STYLESETITALIC, false, false, true, true, true, false, true},
    {SCI_STYLESETSIZE, false, false, true, true, true, false, true},
    {SCI_STYLESETFONT, false, false, true, true, true, false, true},
    {SCI_STYLESETEOLFILLED, false, false, true, true, true, false, false},
    {SCI_STYLESETUNDERLINE, false, false, true, true, true, false, false},
    {SCI_STYLESETSIZEFRACTIONAL, false, false, true, true, true, false, true},
    {SCI_STYLEGETSIZEFRACTIONAL, false, true, false, false, false, false, false},
    {SCI_STYLESETWEIGHT, false, false, true, true, true, false, true},
    {SCI_STYLEGETWEIGHT, false, true, false, false, false, false, false},
    {SCI_STYLESETCHARACTERSET, false, false, true, true, true, false, true},
    {SCI_SETSELFORE, false, false, true, false, false, false, false},
    {SCI_SETSELBACK, false, false, true, false, false, false, false},
    {SCI_SETCARETFORE, false, false, true, false, false, false, false},
    {SCI_STYLESETVISIBLE, false, false, true, true, true, false, false},
    {SCI_GETCARETPERIOD, true, true, false, false, false, false, false},
    {SCI_INDICGETSTYLE, false, true, false, false, false, false, false},
    {SCI_INDICGETFORE, false, true, false, false, false, false, false},
    {SCI_GETWHITESPACESIZE, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONHIDDEN, true, true, false, false, false, false, false},
#ifdef SCI_GETSTYLEBITS
    {SCI_GETSTYLEBITS, true, true, false, false, false, false, false},
#endif
    {SCI_GETLINESTATE, true, true, false, false, false, false, false},
    {SCI_GETMAXLINESTATE, true, true, false, false, false, false, false},
    {SCI_GETCARETLINEVISIBLE, true, true, false, false, false, false, false},
    {SCI_SETCARETLINEVISIBLE, false, false, true, false, false, false, false},
    {SCI_GETCARETLINEBACK, true, true, false, false, false, false, false},
    {SCI_SETCARETLINEBACK, false, false, true, false, false, false, false},
    {SCI_AUTOCACTIVE, false, true, false, false, false, false, false},
    {SCI_AUTOCPOSSTART, false, true, false, false, false, false, false},
    {SCI_AUTOCGETSEPARATOR, false, true, false, false, false, false, false},
    {SCI_AUTOCGETCANCELATSTART, false, true, false, false, false, false, false},
    {SCI_AUTOCGETCHOOSESINGLE, false, true, false, false, false, false, false},
    {SCI_AUTOCGETIGNORECASE, false, true, false, false, false, false, false},
    {SCI_AUTOCGETAUTOHIDE, false, true, false, false, false, false, false},
    {SCI_GETTABWIDTH, true, true, false, false, false, false, false},
    {SCI_GETINDENT, true, true, false, false, false, false, false},
    {SCI_SETUSETABS, false, false, true, true, true, false, true},
    {SCI_GETUSETABS, true, true, false, false, false, false, false},
    {SCI_GETLINEINDENTATION, true, true, false, false, false, false, false},
    {SCI_GETLINEINDENTPOSITION, true, true, false, false, false, false, false},
    {SCI_GETCOLUMN, true, true, false, false, false, false, false},
    {SCI_GETHSCROLLBAR, true, true, false, false, false, false, false},
    {SCI_GETINDENTATIONGUIDES, true, true, false, false, false, false, false},
    {SCI_GETHIGHLIGHTGUIDE, true, true, false, false, false, false, false},
    {SCI_GETLINEENDPOSITION, true, true, false, false, false, false, false},
    {SCI_GETCODEPAGE, true, true, false, false, false, false, false},
    {SCI_GETCARETFORE, true, true, false, false, false, false, false},
    {SCI_GETREADONLY, true, true, false, false, false, false, false},
    {SCI_SETCURRENTPOS, false, false, true, false, false, false, false},
    {SCI_GETSELECTIONSTART, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONEND, true, true, false, false, false, false, false},
    {SCI_GETPRINTMAGNIFICATION, true, true, false, false, false, false, false},
    {SCI_GETPRINTCOLOURMODE, true, true, false, false, false, false, false},
    {SCI_FINDTEXT, false, true, false, false, false, false, false},
    {SCI_GETFIRSTVISIBLELINE, true, true, false, false, false, false, false},
    {SCI_GETLINE, true, true, false, false, false, false, false},
    {SCI_GETLINECOUNT, true, true, false, false, false, false, false},
    {SCI_SETMARGINLEFT, false, false, true, true, true, false, true},
    {SCI_GETMARGINLEFT, true, true, false, false, false, false, false},
    {SCI_SETMARGINRIGHT, false, false, true, true, true, false, true},
    {SCI_GETMARGINRIGHT, true, true, false, false, false, false, false},
    {SCI_GETMODIFY, true, true, false, false, false, false, false},
    {SCI_SETSEL, false, false, true, false, false, false, false},
    {SCI_GETSELTEXT, true, true, false, false, false, false, false},
    {SCI_GETTEXTRANGE, true, true, false, false, false, false, false},
    {SCI_POINTXFROMPOSITION, false, true, false, false, false, false, false},
    {SCI_POINTYFROMPOSITION, false, true, false, false, false, false, false},
    {SCI_LINEFROMPOSITION, false, true, false, false, false, false, false},
    {SCI_POSITIONFROMLINE, false, true, false, false, false, false, false},
    {SCI_REPLACESEL, false, false, true, true, true, false, false},
    {SCI_SETREADONLY, false, false, true, true, true, false, false},
    {SCI_CANPASTE, false, true, false, false, false, false, false},
    {SCI_CANUNDO, false, true, false, false, false, false, false},
    {SCI_SETTEXT, false, false, true, true, true, false, true},
    {SCI_GETTEXT, true, true, false, false, false, false, false},
    {SCI_GETTEXTLENGTH, true, true, false, false, false, false, false},
    {SCI_GETDIRECTFUNCTION, true, true, false, false, false, false, false},
    {SCI_GETDIRECTPOINTER, true, true, false, false, false, false, false},
    {SCI_GETOVERTYPE, true, true, false, false, false, false, false},
    {SCI_SETCARETWIDTH, false, false, true, false, false, false, false},
    {SCI_GETCARETWIDTH, true, true, false, false, false, false, false},
    {SCI_GETTARGETSTART, true, true, false, false, false, false, false},
    {SCI_GETTARGETEND, true, true, false, false, false, false, false},
    {SCI_FINDTEXTFULL, false, true, false, false, false, false, false},
    {SCI_GETSEARCHFLAGS, true, true, false, false, false, false, false},
    {SCI_CALLTIPACTIVE, false, true, false, false, false, false, false},
    {SCI_CALLTIPPOSSTART, false, true, false, false, false, false, false},
    {SCI_AUTOCGETMAXWIDTH, false, true, false, false, false, false, false},
    {SCI_AUTOCGETMAXHEIGHT, false, true, false, false, false, false, false},
    {SCI_VISIBLEFROMDOCLINE, false, true, false, false, false, false, false},
    {SCI_DOCLINEFROMVISIBLE, false, true, false, false, false, false, false},
    {SCI_GETFOLDLEVEL, true, true, false, false, false, false, false},
    {SCI_GETLASTCHILD, true, true, false, false, false, false, false},
    {SCI_GETFOLDPARENT, true, true, false, false, false, false, false},
    {SCI_GETLINEVISIBLE, true, true, false, false, false, false, false},
    {SCI_GETFOLDEXPANDED, true, true, false, false, false, false, false},
    {SCI_WRAPCOUNT, false, true, false, false, false, false, false},
    {SCI_GETALLLINESVISIBLE, true, true, false, false, false, false, false},
    {SCI_SETMARGINTYPEN, false, false, true, true, true, false, false},
    {SCI_GETMARGINTYPEN, true, true, false, false, false, false, false},
    {SCI_SETMARGINWIDTHN, false, false, true, true, true, false, true},
    {SCI_GETMARGINWIDTHN, true, true, false, false, false, false, false},
    {SCI_SETMARGINMASKN, false, false, true, true, true, false, false},
    {SCI_GETMARGINMASKN, true, true, false, false, false, false, false},
    {SCI_SETMARGINSENSITIVEN, false, false, true, true, true, false, false},
    {SCI_GETMARGINSENSITIVEN, true, true, false, false, false, false, false},
    {SCI_GETMARGINCURSORN, true, true, false, false, false, false, false},
    {SCI_GETMARGINBACKN, true, true, false, false, false, false, false},
    {SCI_GETMARGINS, true, true, false, false, false, false, false},
    {SCI_STYLEGETCHECKMONOSPACED, false, true, false, false, false, false, false},
    {SCI_STYLEGETINVISIBLEREPRESENTATION, false, true, false, false, false, false, false},
    {SCI_GETTABINDENTS, true, true, false, false, false, false, false},
    {SCI_GETBACKSPACEUNINDENTS, true, true, false, false, false, false, false},
    {SCI_GETMOUSEDWELLTIME, true, true, false, false, false, false, false},
    {SCI_WORDSTARTPOSITION, false, true, false, false, false, false, false},
    {SCI_WORDENDPOSITION, false, true, false, false, false, false, false},
    {SCI_SETWRAPMODE, false, false, true, true, true, false, true},
    {SCI_GETWRAPMODE, true, true, false, false, false, false, false},
    {SCI_AUTOCGETDROPRESTOFWORD, false, true, false, false, false, false, false},
    {SCI_GETLAYOUTCACHE, true, true, false, false, false, false, false},
    {SCI_SETSCROLLWIDTH, false, false, true, true, true, false, false},
    {SCI_GETSCROLLWIDTH, true, true, false, false, false, false, false},
    {SCI_TEXTWIDTH, false, true, false, false, false, false, false},
    {SCI_GETENDATLASTLINE, true, true, false, false, false, false, false},
    {SCI_TEXTHEIGHT, false, true, false, false, false, false, false},
    {SCI_GETVSCROLLBAR, true, true, false, false, false, false, false},
    {SCI_APPENDTEXT, false, false, true, true, true, false, false},
#ifdef SCI_GETTWOPHASEDRAW
    {SCI_GETTWOPHASEDRAW, true, true, false, false, false, false, false},
#endif
    {SCI_AUTOCGETTYPESEPARATOR, false, true, false, false, false, false, false},
    {SCI_MARKERSETBACKSELECTED, false, false, true, true, true, false, false},
    {SCI_MARKERENABLEHIGHLIGHT, false, false, true, true, true, false, false},
    {SCI_MARKERSETFORETRANSLUCENT, false, false, true, true, true, false, false},
    {SCI_MARKERSETBACKTRANSLUCENT, false, false, true, true, true, false, false},
    {SCI_MARKERSETBACKSELECTEDTRANSLUCENT, false, false, true, true, true, false, false},
    {SCI_LINEDOWN, false, false, true, false, false, false, false},
    {SCI_LINEDOWNEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEUP, false, false, true, false, false, false, false},
    {SCI_LINEUPEXTEND, false, false, true, false, false, false, false},
    {SCI_CHARLEFT, false, false, true, false, false, false, false},
    {SCI_CHARLEFTEXTEND, false, false, true, false, false, false, false},
    {SCI_CHARRIGHT, false, false, true, false, false, false, false},
    {SCI_CHARRIGHTEXTEND, false, false, true, false, false, false, false},
    {SCI_WORDLEFT, false, false, true, false, false, false, false},
    {SCI_WORDLEFTEXTEND, false, false, true, false, false, false, false},
    {SCI_WORDRIGHT, false, false, true, false, false, false, false},
    {SCI_WORDRIGHTEXTEND, false, false, true, false, false, false, false},
    {SCI_HOME, false, false, true, false, false, false, false},
    {SCI_HOMEEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEEND, false, false, true, false, false, false, false},
    {SCI_LINEENDEXTEND, false, false, true, false, false, false, false},
    {SCI_DOCUMENTSTART, false, false, true, false, false, false, false},
    {SCI_DOCUMENTSTARTEXTEND, false, false, true, false, false, false, false},
    {SCI_DOCUMENTEND, false, false, true, false, false, false, false},
    {SCI_DOCUMENTENDEXTEND, false, false, true, false, false, false, false},
    {SCI_PAGEUP, false, false, true, false, false, false, false},
    {SCI_PAGEUPEXTEND, false, false, true, false, false, false, false},
    {SCI_PAGEDOWN, false, false, true, false, false, false, false},
    {SCI_PAGEDOWNEXTEND, false, false, true, false, false, false, false},
    {SCI_VCHOME, false, false, true, false, false, false, false},
    {SCI_VCHOMEEXTEND, false, false, true, false, false, false, false},
    {SCI_HOMEDISPLAY, false, false, true, false, false, false, false},
    {SCI_HOMEDISPLAYEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEENDDISPLAY, false, false, true, false, false, false, false},
    {SCI_LINEENDDISPLAYEXTEND, false, false, true, false, false, false, false},
    {SCI_HOMEWRAP, false, false, true, false, false, false, false},
    {SCI_LINELENGTH, false, true, false, false, false, false, false},
    {SCI_BRACEMATCH, false, true, false, false, false, false, false},
    {SCI_GETVIEWEOL, true, true, false, false, false, false, false},
    {SCI_GETDOCPOINTER, true, true, false, false, false, false, false},
    {SCI_SETDOCPOINTER, false, false, true, true, true, false, true},
    {SCI_GETEDGECOLUMN, true, true, false, false, false, false, false},
    {SCI_GETEDGEMODE, true, true, false, false, false, false, false},
    {SCI_GETEDGECOLOUR, true, true, false, false, false, false, false},
    {SCI_BRACEMATCHNEXT, false, true, false, false, false, false, false},
    {SCI_LINESONSCREEN, false, true, false, false, false, false, false},
    {SCI_SELECTIONISRECTANGLE, false, true, false, false, false, false, false},
    {SCI_GETZOOM, true, true, false, false, false, false, false},
    {SCI_GETMODEVENTMASK, true, true, false, false, false, false, false},
    {SCI_GETDOCUMENTOPTIONS, true, true, false, false, false, false, false},
    {SCI_GETFOCUS, true, true, false, false, false, false, false},
    {SCI_GETSTATUS, true, true, false, false, false, false, false},
    {SCI_GETMOUSEDOWNCAPTURES, true, true, false, false, false, false, false},
    {SCI_GETCURSOR, true, true, false, false, false, false, false},
    {SCI_GETCONTROLCHARSYMBOL, true, true, false, false, false, false, false},
    {SCI_WORDPARTLEFT, false, false, true, false, false, false, false},
    {SCI_WORDPARTLEFTEXTEND, false, false, true, false, false, false, false},
    {SCI_WORDPARTRIGHT, false, false, true, false, false, false, false},
    {SCI_WORDPARTRIGHTEXTEND, false, false, true, false, false, false, false},
    {SCI_SETXOFFSET, false, false, true, true, false, false, false},
    {SCI_GETXOFFSET, true, true, false, false, false, false, false},
    {SCI_GETPRINTWRAPMODE, true, true, false, false, false, false, false},
    {SCI_POSITIONBEFORE, false, true, false, false, false, false, false},
    {SCI_POSITIONAFTER, false, true, false, false, false, false, false},
    {SCI_GETSELECTIONMODE, true, true, false, false, false, false, false},
    {SCI_GETLINESELSTARTPOSITION, true, true, false, false, false, false, false},
    {SCI_GETLINESELENDPOSITION, true, true, false, false, false, false, false},
    {SCI_LINEDOWNRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEUPRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_CHARLEFTRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_CHARRIGHTRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_HOMERECTEXTEND, false, false, true, false, false, false, false},
    {SCI_VCHOMERECTEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEENDRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_PAGEUPRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_PAGEDOWNRECTEXTEND, false, false, true, false, false, false, false},
    {SCI_STUTTEREDPAGEUP, false, false, true, false, false, false, false},
    {SCI_STUTTEREDPAGEUPEXTEND, false, false, true, false, false, false, false},
    {SCI_STUTTEREDPAGEDOWN, false, false, true, false, false, false, false},
    {SCI_STUTTEREDPAGEDOWNEXTEND, false, false, true, false, false, false, false},
    {SCI_WORDLEFTEND, false, false, true, false, false, false, false},
    {SCI_WORDLEFTENDEXTEND, false, false, true, false, false, false, false},
    {SCI_WORDRIGHTEND, false, false, true, false, false, false, false},
    {SCI_WORDRIGHTENDEXTEND, false, false, true, false, false, false, false},
    {SCI_AUTOCGETCURRENT, false, true, false, false, false, false, false},
    {SCI_TARGETASUTF8, false, true, false, false, false, false, false},
    {SCI_ENCODEDFROMUTF8, false, true, false, false, false, false, false},
    {SCI_HOMEWRAPEXTEND, false, false, true, false, false, false, false},
    {SCI_LINEENDWRAP, false, false, true, false, false, false, false},
    {SCI_LINEENDWRAPEXTEND, false, false, true, false, false, false, false},
    {SCI_VCHOMEWRAP, false, false, true, false, false, false, false},
    {SCI_VCHOMEWRAPEXTEND, false, false, true, false, false, false, false},
    {SCI_FINDCOLUMN, false, true, false, false, false, false, false},
    {SCI_GETCARETSTICKY, true, true, false, false, false, false, false},
    {SCI_GETWRAPVISUALFLAGS, true, true, false, false, false, false, false},
    {SCI_GETWRAPVISUALFLAGSLOCATION, true, true, false, false, false, false, false},
    {SCI_GETWRAPSTARTINDENT, true, true, false, false, false, false, false},
    {SCI_MARKERADDSET, false, false, true, true, true, false, false},
    {SCI_GETPASTECONVERTENDINGS, true, true, false, false, false, false, false},
    {SCI_GETCARETLINEBACKALPHA, true, true, false, false, false, false, false},
    {SCI_GETWRAPINDENTMODE, true, true, false, false, false, false, false},
    {SCI_GETSELALPHA, true, true, false, false, false, false, false},
    {SCI_GETSELEOLFILLED, true, true, false, false, false, false, false},
    {SCI_STYLEGETFORE, false, true, false, false, false, false, false},
    {SCI_STYLEGETBACK, false, true, false, false, false, false, false},
    {SCI_STYLEGETBOLD, false, true, false, false, false, false, false},
    {SCI_STYLEGETITALIC, false, true, false, false, false, false, false},
    {SCI_STYLEGETSIZE, false, true, false, false, false, false, false},
    {SCI_STYLEGETFONT, false, true, false, false, false, false, false},
    {SCI_STYLEGETEOLFILLED, false, true, false, false, false, false, false},
    {SCI_STYLEGETUNDERLINE, false, true, false, false, false, false, false},
    {SCI_STYLEGETCASE, false, true, false, false, false, false, false},
    {SCI_STYLEGETCHARACTERSET, false, true, false, false, false, false, false},
    {SCI_STYLEGETVISIBLE, false, true, false, false, false, false, false},
    {SCI_STYLEGETCHANGEABLE, false, true, false, false, false, false, false},
    {SCI_STYLEGETHOTSPOT, false, true, false, false, false, false, false},
    {SCI_GETHOTSPOTACTIVEFORE, true, true, false, false, false, false, false},
    {SCI_GETHOTSPOTACTIVEBACK, true, true, false, false, false, false, false},
    {SCI_GETHOTSPOTACTIVEUNDERLINE, true, true, false, false, false, false, false},
    {SCI_GETHOTSPOTSINGLELINE, true, true, false, false, false, false, false},
    {SCI_GETINDICATORCURRENT, true, true, false, false, false, false, false},
    {SCI_GETINDICATORVALUE, true, true, false, false, false, false, false},
    {SCI_INDICATORALLONFOR, false, true, false, false, false, false, false},
    {SCI_INDICATORVALUEAT, false, true, false, false, false, false, false},
    {SCI_INDICATORSTART, false, true, false, false, false, false, false},
    {SCI_INDICATOREND, false, true, false, false, false, false, false},
    {SCI_INDICGETUNDER, false, true, false, false, false, false, false},
    {SCI_GETCARETSTYLE, true, true, false, false, false, false, false},
    {SCI_GETPOSITIONCACHE, true, true, false, false, false, false, false},
    {SCI_GETSCROLLWIDTHTRACKING, true, true, false, false, false, false, false},
    {SCI_GETCHARACTERPOINTER, true, true, false, false, false, false, false},
#ifdef SCI_GETKEYSUNICODE
    {SCI_GETKEYSUNICODE, true, true, false, false, false, false, false},
#endif
    {SCI_INDICGETALPHA, false, true, false, false, false, false, false},
    {SCI_GETEXTRAASCENT, true, true, false, false, false, false, false},
    {SCI_GETEXTRADESCENT, true, true, false, false, false, false, false},
    {SCI_MARKERSYMBOLDEFINED, false, true, false, false, false, false, false},
    {SCI_MARGINGETTEXT, false, true, false, false, false, false, false},
    {SCI_MARGINGETSTYLE, false, true, false, false, false, false, false},
    {SCI_MARGINGETSTYLES, false, true, false, false, false, false, false},
    {SCI_MARGINGETSTYLEOFFSET, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETTEXT, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETSTYLE, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETSTYLES, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETLINES, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETVISIBLE, false, true, false, false, false, false, false},
    {SCI_ANNOTATIONGETSTYLEOFFSET, false, true, false, false, false, false, false},
    {SCI_SETEMPTYSELECTION, false, false, true, false, false, false, false},
    {SCI_GETMARGINOPTIONS, true, true, false, false, false, false, false},
    {SCI_INDICGETOUTLINEALPHA, false, true, false, false, false, false, false},
    {SCI_CHARPOSITIONFROMPOINT, false, true, false, false, false, false, false},
    {SCI_CHARPOSITIONFROMPOINTCLOSE, false, true, false, false, false, false, false},
    {SCI_GETMULTIPLESELECTION, true, true, false, false, false, false, false},
    {SCI_GETADDITIONALSELECTIONTYPING, true, true, false, false, false, false, false},
    {SCI_GETADDITIONALCARETSBLINK, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONS, true, true, false, false, false, false, false},
    {SCI_GETMAINSELECTION, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNCARET, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNANCHOR, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNCARETVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNANCHORVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNSTART, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNEND, true, true, false, false, false, false, false},
    {SCI_GETRECTANGULARSELECTIONCARET, true, true, false, false, false, false, false},
    {SCI_GETRECTANGULARSELECTIONANCHOR, true, true, false, false, false, false, false},
    {SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETVIRTUALSPACEOPTIONS, true, true, false, false, false, false, false},
    {SCI_GETRECTANGULARSELECTIONMODIFIER, true, true, false, false, false, false, false},
    {SCI_GETADDITIONALSELALPHA, true, true, false, false, false, false, false},
    {SCI_GETADDITIONALCARETFORE, true, true, false, false, false, false, false},
    {SCI_GETADDITIONALCARETSVISIBLE, true, true, false, false, false, false, false},
    {SCI_AUTOCGETCURRENTTEXT, false, true, false, false, false, false, false},
    {SCI_GETFONTQUALITY, true, true, false, false, false, false, false},
    {SCI_SETFIRSTVISIBLELINE, false, false, true, true, true, true, false},
    {SCI_GETMULTIPASTE, true, true, false, false, false, false, false},
    {SCI_GETTAG, true, true, false, false, false, false, false},
    {SCI_CONTRACTEDFOLDNEXT, false, true, false, false, false, false, false},
    {SCI_GETIDENTIFIER, true, true, false, false, false, false, false},
    {SCI_MARKERDEFINERGBAIMAGE, false, false, true, true, true, false, false},
    {SCI_GETTECHNOLOGY, true, true, false, false, false, false, false},
    {SCI_COUNTCHARACTERS, false, true, false, false, false, false, false},
    {SCI_AUTOCGETCASEINSENSITIVEBEHAVIOUR, false, true, false, false, false, false, false},
    {SCI_AUTOCGETMULTI, false, true, false, false, false, false, false},
    {SCI_AUTOCGETOPTIONS, false, true, false, false, false, false, false},
    {SCI_GETRANGEPOINTER, true, true, false, false, false, false, false},
    {SCI_GETGAPPOSITION, true, true, false, false, false, false, false},
    {SCI_DELETERANGE, false, false, true, true, true, false, false},
    {SCI_GETWORDCHARS, true, true, false, false, false, false, false},
    {SCI_GETWHITESPACECHARS, true, true, false, false, false, false, false},
    {SCI_GETPUNCTUATIONCHARS, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONEMPTY, true, true, false, false, false, false, false},
    {SCI_VCHOMEDISPLAY, false, false, true, false, false, false, false},
    {SCI_VCHOMEDISPLAYEXTEND, false, false, true, false, false, false, false},
    {SCI_GETCARETLINEVISIBLEALWAYS, true, true, false, false, false, false, false},
    {SCI_GETLINEENDTYPESALLOWED, true, true, false, false, false, false, false},
    {SCI_GETLINEENDTYPESACTIVE, true, true, false, false, false, false, false},
    {SCI_AUTOCGETORDER, false, true, false, false, false, false, false},
    {SCI_GETAUTOMATICFOLD, true, true, false, false, false, false, false},
    {SCI_GETREPRESENTATION, true, true, false, false, false, false, false},
    {SCI_GETMOUSESELECTIONRECTANGULARSWITCH, true, true, false, false, false, false, false},
    {SCI_POSITIONRELATIVE, false, true, false, false, false, false, false},
    {SCI_GETPHASESDRAW, true, true, false, false, false, false, false},
    {SCI_GETNEXTTABSTOP, true, true, false, false, false, false, false},
    {SCI_GETIMEINTERACTION, true, true, false, false, false, false, false},
    {SCI_INDICGETHOVERSTYLE, false, true, false, false, false, false, false},
    {SCI_INDICGETHOVERFORE, false, true, false, false, false, false, false},
    {SCI_INDICGETFLAGS, false, true, false, false, false, false, false},
    {SCI_GETTARGETTEXT, true, true, false, false, false, false, false},
    {SCI_ISRANGEWORD, false, true, false, false, false, false, false},
    {SCI_GETIDLESTYLING, true, true, false, false, false, false, false},
    {SCI_GETMOUSEWHEELCAPTURES, true, true, false, false, false, false, false},
    {SCI_GETTABDRAWMODE, true, true, false, false, false, false, false},
    {SCI_GETACCESSIBILITY, true, true, false, false, false, false, false},
    {SCI_GETCARETLINEFRAME, true, true, false, false, false, false, false},
    {SCI_GETMOVEEXTENDSSELECTION, true, true, false, false, false, false, false},
    {SCI_FOLDDISPLAYTEXTGETSTYLE, true, true, false, false, false, false, false},
    {SCI_GETBIDIRECTIONAL, true, true, false, false, false, false, false},
    {SCI_SETBIDIRECTIONAL, false, false, true, true, true, false, false},
    {SCI_GETLINECHARACTERINDEX, true, true, false, false, false, false, false},
    {SCI_LINEFROMINDEXPOSITION, false, true, false, false, false, false, false},
    {SCI_INDEXPOSITIONFROMLINE, false, true, false, false, false, false, false},
    {SCI_COUNTCODEUNITS, false, true, false, false, false, false, false},
    {SCI_POSITIONRELATIVECODEUNITS, false, true, false, false, false, false, false},
    {SCI_GETCOMMANDEVENTS, true, true, false, false, false, false, false},
    {SCI_GETCHARACTERCATEGORYOPTIMIZATION, true, true, false, false, false, false, false},
    {SCI_GETDEFAULTFOLDDISPLAYTEXT, true, true, false, false, false, false, false},
    {SCI_GETTABMINIMUMWIDTH, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNSTARTVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONNENDVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETTARGETSTARTVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_GETTARGETENDVIRTUALSPACE, true, true, false, false, false, false, false},
    {SCI_MARKERHANDLEFROMLINE, false, true, false, false, false, false, false},
    {SCI_MARKERNUMBERFROMLINE, false, true, false, false, false, false, false},
    {SCI_MARKERGETLAYER, false, true, false, false, false, false, false},
    {SCI_EOLANNOTATIONGETTEXT, false, true, false, false, false, false, false},
    {SCI_EOLANNOTATIONGETSTYLE, false, true, false, false, false, false, false},
    {SCI_EOLANNOTATIONGETVISIBLE, false, true, false, false, false, false, false},
    {SCI_EOLANNOTATIONGETSTYLEOFFSET, false, true, false, false, false, false, false},
    {SCI_GETMULTIEDGECOLUMN, true, true, false, false, false, false, false},
    {SCI_SUPPORTSFEATURE, false, true, false, false, false, false, false},
    {SCI_INDICGETSTROKEWIDTH, false, true, false, false, false, false, false},
    {SCI_GETELEMENTCOLOUR, true, true, false, false, false, false, false},
    {SCI_GETELEMENTISSET, true, true, false, false, false, false, false},
    {SCI_GETELEMENTALLOWSTRANSLUCENT, true, true, false, false, false, false, false},
    {SCI_GETELEMENTBASECOLOUR, true, true, false, false, false, false, false},
    {SCI_GETFONTLOCALE, true, true, false, false, false, false, false},
    {SCI_GETSELECTIONLAYER, true, true, false, false, false, false, false},
    {SCI_GETCARETLINELAYER, true, true, false, false, false, false, false},
    {SCI_GETREPRESENTATIONAPPEARANCE, true, true, false, false, false, false, false},
    {SCI_GETREPRESENTATIONCOLOUR, true, true, false, false, false, false, false},
    {SCI_GETDIRECTSTATUSFUNCTION, true, true, false, false, false, false, false},
    {SCI_GETCARETLINEHIGHLIGHTSUBLINE, true, true, false, false, false, false, false},
    {SCI_GETLAYOUTTHREADS, true, true, false, false, false, false, false},
    {SCI_GETSTYLEDTEXTFULL, true, true, false, false, false, false, false},
    {SCI_GETCHANGEHISTORY, true, true, false, false, false, false, false},
    {SCI_GETLEXER, true, true, false, false, false, false, false},
    {SCI_GETPROPERTY, true, true, false, false, false, false, false},
    {SCI_GETPROPERTYEXPANDED, true, true, false, false, false, false, false},
    {SCI_GETPROPERTYINT, true, true, false, false, false, false, false},
#ifdef SCI_GETSTYLEBITSNEEDED
    {SCI_GETSTYLEBITSNEEDED, true, true, false, false, false, false, false},
#endif
    {SCI_GETLEXERLANGUAGE, true, true, false, false, false, false, false},
    {SCI_PROPERTYNAMES, false, true, false, false, false, false, false},
    {SCI_PROPERTYTYPE, false, true, false, false, false, false, false},
    {SCI_DESCRIBEPROPERTY, false, true, false, false, false, false, false},
    {SCI_DESCRIBEKEYWORDSETS, false, true, false, false, false, false, false},
    {SCI_GETLINEENDTYPESSUPPORTED, true, true, false, false, false, false, false},
    {SCI_GETSUBSTYLESSTART, true, true, false, false, false, false, false},
    {SCI_GETSUBSTYLESLENGTH, true, true, false, false, false, false, false},
    {SCI_DISTANCETOSECONDARYSTYLES, true, true, false, false, false, false, false},
    {SCI_GETSUBSTYLEBASES, true, true, false, false, false, false, false},
    {SCI_GETSTYLEFROMSUBSTYLE, true, true, false, false, false, false, false},
    {SCI_GETPRIMARYSTYLEFROMSTYLE, true, true, false, false, false, false, false},
    {SCI_GETNAMEDSTYLES, true, true, false, false, false, false, false},
    {SCI_NAMEOFSTYLE, false, true, false, false, false, false, false},
    {SCI_TAGSOFSTYLE, false, true, false, false, false, false, false},
    {SCI_DESCRIPTIONOFSTYLE, false, true, false, false, false, false, false},
};

bool legacy_dispatch_rules_are_sorted()
{
    for (std::size_t i = 1; i < sizeof(k_legacy_dispatch_rules) / sizeof(k_legacy_dispatch_rules[0]); ++i) {
        if (k_legacy_dispatch_rules[i - 1].message >= k_legacy_dispatch_rules[i].message) {
            return false;
        }
    }

    return true;
}

const Legacy_dispatch_rule* find_legacy_dispatch_rule(unsigned int message)
{
    std::size_t first = 0;
    std::size_t count = sizeof(k_legacy_dispatch_rules) / sizeof(k_legacy_dispatch_rules[0]);

    while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t index = first + step;
        const Legacy_dispatch_rule& rule = k_legacy_dispatch_rules[index];

        if (rule.message < message) {
            first = index + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    if (first < sizeof(k_legacy_dispatch_rules) / sizeof(k_legacy_dispatch_rules[0])
        && k_legacy_dispatch_rules[first].message == message) {
        return &k_legacy_dispatch_rules[first];
    }

    return nullptr;
}

Legacy_dispatch_rule legacy_dispatch_for_message(unsigned int message)
{
    if (const Legacy_dispatch_rule* rule = find_legacy_dispatch_rule(message)) {
        return *rule;
    }

    return {message, false, false, true, true, true, false, false};
}

void expect_dispatch_field_equal(
    unsigned int message,
    const char* field_name,
    bool actual,
    bool expected)
{
    if (actual != expected) {
        std::fprintf(stderr,
            "FAIL: SCI_ message %u field %s changed: actual=%s expected=%s.\n",
            message,
            field_name,
            actual ? "true" : "false",
            expected ? "true" : "false");
    }

    SQ_EXPECT(actual == expected);
}

void expect_read_only_fast_path(unsigned int msg, const char* group_name)
{
    if (!scene_graph_message_is_known_read_only(msg)) {
        std::fprintf(stderr,
            "FAIL: SCI_ message %u from read-only group '%s' is not "
            "classified as read-only.\n",
            msg,
            group_name);
    }
    SQ_EXPECT(scene_graph_message_is_known_read_only(msg));

    const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
    SQ_EXPECT(!req.needed);
    SQ_EXPECT(!req.static_content_dirty);
    SQ_EXPECT(!req.needs_style_sync);
    SQ_EXPECT(!req.scrolling);
}

template <std::size_t N>
void expect_read_only_group(const char* group_name, const unsigned int (&messages)[N])
{
    for (unsigned int msg : messages) {
        expect_read_only_fast_path(msg, group_name);
    }
}

void expect_read_only_group(const char* group_name, const std::vector<unsigned int>& messages)
{
    for (unsigned int msg : messages) {
        expect_read_only_fast_path(msg, group_name);
    }
}

void test_known_mutating_messages_request_update()
{
    // A few messages from each class of the allow-list. These must all
    // set `needed=true`.
    const unsigned int mutators[] = {
        SCI_SETTEXT,
        SCI_CLEARALL,
        SCI_INSERTTEXT,
        SCI_APPENDTEXT,
        SCI_REPLACESEL,
        SCI_DELETERANGE,
        SCI_STYLECLEARALL,
        SCI_STYLESETFORE,
        SCI_STYLESETBACK,
        SCI_STYLESETFONT,
        SCI_SETWRAPMODE,
        SCI_SETREADONLY,
        SCI_SETFIRSTVISIBLELINE,
        SCI_SETXOFFSET,
        SCI_SETSEL,
        SCI_GOTOPOS,
        SCI_SETCURRENTPOS,
        SCI_SETANCHOR,
        SCI_SETCARETWIDTH,
        SCI_MARKERADD,
        SCI_MARKERDELETE,
    };

    for (unsigned int msg : mutators) {
        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        SQ_EXPECT(req.needed);
    }
}

void test_specific_scroll_dispatch()
{
    // SCI_SETFIRSTVISIBLELINE is the one message that sets `scrolling=true`.
    const scene_graph_update_request_info_t set_first = scene_graph_update_request(SCI_SETFIRSTVISIBLELINE);
    SQ_EXPECT(set_first.needed);
    SQ_EXPECT(set_first.static_content_dirty);
    SQ_EXPECT(set_first.needs_style_sync);
    SQ_EXPECT(set_first.scrolling);

    // SCI_SETXOFFSET dirties static content but is not treated as a
    // vertical scroll.
    const scene_graph_update_request_info_t set_xoff = scene_graph_update_request(SCI_SETXOFFSET);
    SQ_EXPECT(set_xoff.needed);
    SQ_EXPECT(set_xoff.static_content_dirty);
    SQ_EXPECT(!set_xoff.scrolling);
}

// Regression: caret and selection movement commands must be classified
// as "overlay-only" mutators -- needed=true, but static_content_dirty
// and needs_style_sync both false -- so that tight caret-stepping loops
// like `caret_move_right_5000` in the benchmark do not pay a full
// resync per keystroke. Historically these fell through to the
// conservative full-resync default, which made every single caret move
// invoke `syncQuickViewProperties()` and mark the scene graph's static
// content dirty.
void test_caret_movement_dispatch_is_overlay_only()
{
    const unsigned int caret_moves[] = {
        SCI_CHARLEFT,
        SCI_CHARRIGHT,
        SCI_CHARLEFTEXTEND,
        SCI_CHARRIGHTEXTEND,
        SCI_CHARLEFTRECTEXTEND,
        SCI_CHARRIGHTRECTEXTEND,
        SCI_LINEUP,
        SCI_LINEDOWN,
        SCI_LINEUPEXTEND,
        SCI_LINEDOWNEXTEND,
        SCI_LINEUPRECTEXTEND,
        SCI_LINEDOWNRECTEXTEND,
        SCI_WORDLEFT,
        SCI_WORDRIGHT,
        SCI_WORDLEFTEXTEND,
        SCI_WORDRIGHTEXTEND,
        SCI_WORDLEFTEND,
        SCI_WORDRIGHTEND,
        SCI_WORDLEFTENDEXTEND,
        SCI_WORDRIGHTENDEXTEND,
        SCI_WORDPARTLEFT,
        SCI_WORDPARTRIGHT,
        SCI_WORDPARTLEFTEXTEND,
        SCI_WORDPARTRIGHTEXTEND,
        SCI_HOME,
        SCI_HOMEEXTEND,
        SCI_HOMERECTEXTEND,
        SCI_HOMEDISPLAY,
        SCI_HOMEDISPLAYEXTEND,
        SCI_HOMEWRAP,
        SCI_HOMEWRAPEXTEND,
        SCI_VCHOME,
        SCI_VCHOMEEXTEND,
        SCI_VCHOMERECTEXTEND,
        SCI_VCHOMEDISPLAY,
        SCI_VCHOMEDISPLAYEXTEND,
        SCI_VCHOMEWRAP,
        SCI_VCHOMEWRAPEXTEND,
        SCI_LINEEND,
        SCI_LINEENDEXTEND,
        SCI_LINEENDRECTEXTEND,
        SCI_LINEENDDISPLAY,
        SCI_LINEENDDISPLAYEXTEND,
        SCI_LINEENDWRAP,
        SCI_LINEENDWRAPEXTEND,
        SCI_DOCUMENTSTART,
        SCI_DOCUMENTSTARTEXTEND,
        SCI_DOCUMENTEND,
        SCI_DOCUMENTENDEXTEND,
        SCI_PAGEUP,
        SCI_PAGEDOWN,
        SCI_PAGEUPEXTEND,
        SCI_PAGEDOWNEXTEND,
        SCI_PAGEUPRECTEXTEND,
        SCI_PAGEDOWNRECTEXTEND,
        SCI_STUTTEREDPAGEUP,
        SCI_STUTTEREDPAGEDOWN,
        SCI_STUTTEREDPAGEUPEXTEND,
        SCI_STUTTEREDPAGEDOWNEXTEND,
    };

    for (unsigned int msg : caret_moves) {
        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        // Must schedule an update -- the caret/selection moved, so the
        // scene graph still needs a repaint to reposition the caret
        // and selection overlay.
        SQ_EXPECT(req.needed);
        // Must NOT mark static content dirty -- text and styles are
        // unchanged, so `build_render_snapshot()` should take the
        // overlay-only capture path.
        if (req.static_content_dirty) {
            std::fprintf(stderr,
                "FAIL: SCI_ caret-move message %u is classified as "
                "static_content_dirty=true. Caret moves do not change "
                "text or styles and must take the overlay-only capture "
                "path so that tight caret-move loops avoid paying a "
                "full resync per keystroke.\n",
                msg);
        }
        SQ_EXPECT(!req.static_content_dirty);
        SQ_EXPECT(!req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

void test_known_read_only_messages_take_fast_path()
{
    // Internal hot-path queries must all return no update request at
    // all (so that internal getters don't thrash the scene graph).
    const unsigned int hot_path_queries[] = {
        SCI_GETTEXT,
        SCI_GETTEXTLENGTH,
        SCI_GETTEXTRANGE,
        SCI_GETSELTEXT,
        SCI_GETCURRENTPOS,
        SCI_GETANCHOR,
        SCI_GETFIRSTVISIBLELINE,
        SCI_GETLINECOUNT,
        SCI_GETXOFFSET,
        SCI_GETSCROLLWIDTH,
        SCI_GETSTYLEAT,
        SCI_GETCARETWIDTH,
        SCI_GETCARETPERIOD,
        SCI_GETZOOM,
        SCI_GETREADONLY,
        SCI_GETMARGINWIDTHN,
        SCI_GETMARGINTYPEN,
        SCI_GETMARGINMASKN,
        SCI_GETMARGINBACKN,
        // Geometry / layout queries added to fix the recursion crash.
        SCI_TEXTHEIGHT,
        SCI_TEXTWIDTH,
        SCI_LINESONSCREEN,
        SCI_LINEFROMPOSITION,
        SCI_POSITIONFROMPOINT,
        // Style queries added for the same reason (they are called
        // from the per-style font / colour sync helper).
        SCI_STYLEGETFORE,
        SCI_STYLEGETBACK,
        SCI_STYLEGETSIZE,
        SCI_STYLEGETSIZEFRACTIONAL,
        SCI_STYLEGETWEIGHT,
        SCI_STYLEGETITALIC,
        SCI_STYLEGETUNDERLINE,
        SCI_STYLEGETFONT,
    };

    for (unsigned int msg : hot_path_queries) {
        SQ_EXPECT(scene_graph_message_is_known_read_only(msg));

        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        SQ_EXPECT(!req.needed);
        SQ_EXPECT(!req.static_content_dirty);
        SQ_EXPECT(!req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

void test_public_query_messages_take_fast_path()
{
    // Public callers also use `send()` directly for a much broader set
    // of getter-style queries. These must not trigger fake content
    // invalidation or full repaint work simply because they are not on
    // the tiny internal hot-path list.
    const unsigned int public_queries[] = {
        SCI_GETLENGTH,
        SCI_GETMODEVENTMASK,
        SCI_GETSELECTIONS,
        SCI_GETSTYLEDTEXTFULL,
        SCI_GETDIRECTPOINTER,
        SCI_GETREPRESENTATION,
        SCI_GETBIDIRECTIONAL,
        SCI_CANUNDO,
        SCI_CANPASTE,
        SCI_CANREDO,
        SCI_POINTXFROMPOSITION,
        SCI_POINTYFROMPOSITION,
        SCI_POSITIONFROMPOINTCLOSE,
        SCI_BRACEMATCH,
        SCI_POSITIONRELATIVE,
        SCI_CHARPOSITIONFROMPOINT,
        SCI_COUNTCHARACTERS,
        SCI_COUNTCODEUNITS,
        // Line / position lookups that are not named SCI_GET*. These
        // must take the fast path: if they fall through to the
        // conservative default, send() marks the scene graph dirty and
        // defeats the vertical-scroll reuse fast path in
        // `build_render_snapshot()`. This is a performance regression
        // test as much as a correctness one -- the scroll reuse buffer
        // and its unit test in the embedded benchmark both rely on
        // these staying on the allow-list.
        SCI_POSITIONFROMLINE,
        SCI_LINELENGTH,
        SCI_VISIBLEFROMDOCLINE,
        SCI_DOCLINEFROMVISIBLE,
        SCI_WRAPCOUNT,
        // Non-SCI_GET query families commonly used through the direct
        // function API. These are pure reads and must not schedule
        // repaint/property-sync work.
        SCI_MARKERGET,
        SCI_MARKERNUMBERFROMLINE,
        SCI_MARKERGETLAYER,
        SCI_MARKERSYMBOLDEFINED,
        SCI_STYLEGETBOLD,
        SCI_STYLEGETEOLFILLED,
        SCI_STYLEGETCASE,
        SCI_STYLEGETCHARACTERSET,
        SCI_STYLEGETVISIBLE,
        SCI_STYLEGETCHANGEABLE,
        SCI_STYLEGETHOTSPOT,
        SCI_STYLEGETCHECKMONOSPACED,
        SCI_STYLEGETINVISIBLEREPRESENTATION,
        SCI_INDICGETSTYLE,
        SCI_INDICGETFORE,
        SCI_INDICGETUNDER,
        SCI_INDICGETHOVERSTYLE,
        SCI_INDICGETHOVERFORE,
        SCI_INDICGETFLAGS,
        SCI_INDICGETSTROKEWIDTH,
        SCI_INDICGETALPHA,
        SCI_INDICGETOUTLINEALPHA,
        SCI_AUTOCGETSEPARATOR,
        SCI_AUTOCGETCANCELATSTART,
        SCI_AUTOCGETCHOOSESINGLE,
        SCI_AUTOCGETIGNORECASE,
        SCI_AUTOCGETAUTOHIDE,
        SCI_AUTOCGETOPTIONS,
        SCI_AUTOCGETDROPRESTOFWORD,
        SCI_AUTOCGETTYPESEPARATOR,
        SCI_AUTOCGETMAXWIDTH,
        SCI_AUTOCGETMAXHEIGHT,
        SCI_AUTOCGETCURRENT,
        SCI_AUTOCGETCURRENTTEXT,
        SCI_AUTOCGETCASEINSENSITIVEBEHAVIOUR,
        SCI_AUTOCGETMULTI,
        SCI_AUTOCGETORDER,
    };

    for (unsigned int msg : public_queries) {
        SQ_EXPECT(scene_graph_message_is_known_read_only(msg));

        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        SQ_EXPECT(!req.needed);
        SQ_EXPECT(!req.static_content_dirty);
        SQ_EXPECT(!req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

void test_direct_query_families_take_fast_path()
{
    const unsigned int fold_and_selection_queries[] = {
        SCI_FOLDDISPLAYTEXTGETSTYLE,
        SCI_SELECTIONISRECTANGLE,
    };
    expect_read_only_group("fold/selection", fold_and_selection_queries);

    const unsigned int margin_queries[] = {
        SCI_MARGINGETTEXT,
        SCI_MARGINGETSTYLE,
        SCI_MARGINGETSTYLES,
        SCI_MARGINGETSTYLEOFFSET,
    };
    expect_read_only_group("margin", margin_queries);

    const unsigned int annotation_queries[] = {
        SCI_ANNOTATIONGETTEXT,
        SCI_ANNOTATIONGETSTYLE,
        SCI_ANNOTATIONGETSTYLES,
        SCI_ANNOTATIONGETLINES,
        SCI_ANNOTATIONGETVISIBLE,
        SCI_ANNOTATIONGETSTYLEOFFSET,
    };
    expect_read_only_group("annotation", annotation_queries);

    const unsigned int eol_annotation_queries[] = {
        SCI_EOLANNOTATIONGETTEXT,
        SCI_EOLANNOTATIONGETSTYLE,
        SCI_EOLANNOTATIONGETVISIBLE,
        SCI_EOLANNOTATIONGETSTYLEOFFSET,
    };
    expect_read_only_group("eol annotation", eol_annotation_queries);

    const unsigned int feature_and_substyle_queries[] = {
        SCI_SUPPORTSFEATURE,
        SCI_DISTANCETOSECONDARYSTYLES,
    };
    expect_read_only_group("feature/substyle", feature_and_substyle_queries);

    std::vector<unsigned int> deprecated_pure_getters;
#ifdef SCI_GETSTYLEBITS
    deprecated_pure_getters.push_back(SCI_GETSTYLEBITS);
#endif
#ifdef SCI_GETSTYLEBITSNEEDED
    deprecated_pure_getters.push_back(SCI_GETSTYLEBITSNEEDED);
#endif
#ifdef SCI_GETKEYSUNICODE
    deprecated_pure_getters.push_back(SCI_GETKEYSUNICODE);
#endif
#ifdef SCI_GETTWOPHASEDRAW
    deprecated_pure_getters.push_back(SCI_GETTWOPHASEDRAW);
#endif
    expect_read_only_group("deprecated pure getters", deprecated_pure_getters);

    const unsigned int marker_lookup_queries[] = {
        SCI_MARKERLINEFROMHANDLE,
        SCI_MARKERHANDLEFROMLINE,
        SCI_MARKERNEXT,
        SCI_MARKERPREVIOUS,
    };
    expect_read_only_group("marker lookup", marker_lookup_queries);

    const unsigned int autocomplete_and_calltip_queries[] = {
        SCI_AUTOCACTIVE,
        SCI_AUTOCPOSSTART,
        SCI_CALLTIPACTIVE,
        SCI_CALLTIPPOSSTART,
    };
    expect_read_only_group("autocomplete/calltip status", autocomplete_and_calltip_queries);

    const unsigned int position_word_and_index_queries[] = {
        SCI_ISRANGEWORD,
        SCI_POSITIONBEFORE,
        SCI_POSITIONAFTER,
        SCI_FINDCOLUMN,
        SCI_LINEFROMINDEXPOSITION,
        SCI_INDEXPOSITIONFROMLINE,
    };
    expect_read_only_group("position/word/index", position_word_and_index_queries);

    const unsigned int indicator_range_queries[] = {
        SCI_INDICATORALLONFOR,
        SCI_INDICATORVALUEAT,
        SCI_INDICATORSTART,
        SCI_INDICATOREND,
    };
    expect_read_only_group("indicator range/value", indicator_range_queries);

    const unsigned int fold_lookup_queries[] = {
        SCI_CONTRACTEDFOLDNEXT,
    };
    expect_read_only_group("fold lookup", fold_lookup_queries);

    const unsigned int lexer_property_queries[] = {
        SCI_PROPERTYNAMES,
        SCI_PROPERTYTYPE,
        SCI_DESCRIBEPROPERTY,
        SCI_DESCRIBEKEYWORDSETS,
        SCI_NAMEOFSTYLE,
        SCI_TAGSOFSTYLE,
        SCI_DESCRIPTIONOFSTYLE,
    };
    expect_read_only_group("lexer/property description", lexer_property_queries);

    const unsigned int encoding_conversion_queries[] = {
        SCI_TARGETASUTF8,
        SCI_ENCODEDFROMUTF8,
    };
    expect_read_only_group("encoding conversion", encoding_conversion_queries);
}

// Regression: every SCI_* message that ScintillaQuick_item issues
// internally via `send()` from inside `syncQuickViewProperties()` or a
// getter called by it MUST take the fast path, otherwise `send()` will
// re-enter `syncQuickViewProperties()` and recurse until the stack
// overflows.
//
// This is not the same test as `test_known_read_only_messages_take_fast_path`
// above: that one enumerates the entire allow-list. This one enumerates
// the EXACT set of messages that the library itself currently sends from
// the re-entry path, so that a future edit which adds a new internal
// query and forgets to put it on the allow-list trips this test
// immediately with a clear failure message instead of crashing one of
// the integration tests.
//
// If you add a new `send(SCI_FOO)` call inside syncQuickViewProperties()
// or anything it calls, add SCI_FOO here too.
void test_sync_quick_view_properties_path_is_recursion_safe()
{
    const unsigned int sync_path_messages[] = {
        SCI_TEXTHEIGHT,          // getCharHeight
        SCI_TEXTWIDTH,           // getCharWidth
        SCI_GETLINECOUNT,        // syncQuickViewProperties (direct)
        SCI_GETSCROLLWIDTH,      // syncQuickViewProperties (direct)
        SCI_LINESONSCREEN,       // syncQuickViewProperties (direct) and getVisibleLines()
        SCI_GETFIRSTVISIBLELINE, // syncQuickViewProperties (direct)
        SCI_GETXOFFSET,          // getFirstVisibleColumn
    };

    for (unsigned int msg : sync_path_messages) {
        SQ_EXPECT(scene_graph_message_is_known_read_only(msg));
        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        if (req.needed) {
            // Log the failing message so that a future regression
            // surfaces with a concrete SCI_ id instead of an opaque
            // assertion failure.
            std::fprintf(stderr,
                "FAIL: SCI_ message %u requests a scene-graph resync "
                "from the sync-quick-view-properties call path and "
                "will recurse via send(). Add it to "
                "`scene_graph_message_is_known_read_only()` in "
                "src/core/scintillaquick_dispatch_table.h.\n",
                msg);
        }
        SQ_EXPECT(!req.needed);
    }
}

void test_unknown_messages_trigger_conservative_full_resync()
{
    // This is THE correctness property the new default is protecting.
    //
    // Use raw synthetic message numbers that are clearly outside
    // Scintilla's SCI_START..SCI_LASTKEYMASK message range. If Scintilla
    // upstream ever extends the message range to cover one of these
    // numbers, update the test; but the property being tested
    // (unknown -> full resync) still holds regardless.
    const unsigned int unknown_messages[] = {
        0xFFFFu,
        0xFFFEu,
        0xFFFDu,
    };

    for (unsigned int msg : unknown_messages) {
        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);
        SQ_EXPECT(req.needed);
        SQ_EXPECT(req.static_content_dirty);
        SQ_EXPECT(req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

void test_read_only_and_mutating_lists_are_disjoint()
{
    // Sanity check: a message cannot be both "known read-only" and an
    // explicit mutator. If this ever fires, one of the two lists has
    // been corrupted.
    const unsigned int mutators[] = {
        SCI_SETTEXT,
        SCI_CLEARALL,
        SCI_STYLECLEARALL,
        SCI_SETWRAPMODE,
        SCI_SETREADONLY,
        SCI_SETFIRSTVISIBLELINE,
        SCI_SETXOFFSET,
    };

    for (unsigned int msg : mutators) {
        SQ_EXPECT(!scene_graph_message_is_known_read_only(msg));
    }
}

void test_stateful_query_like_messages_are_not_read_only()
{
    const unsigned int stateful_messages[] = {
        SCI_SEARCHNEXT,
        SCI_SEARCHPREV,
        SCI_SEARCHINTARGET,
        SCI_REPLACETARGET,
        SCI_REPLACETARGETRE,
        SCI_REPLACETARGETMINIMAL,
        SCI_FORMATRANGE,
        SCI_FORMATRANGEFULL,
        SCI_ALLOCATE,
        SCI_ALLOCATELINES,
        SCI_ALLOCATEEXTENDEDSTYLES,
        SCI_ALLOCATESUBSTYLES,
        SCI_ALLOCATELINECHARACTERINDEX,
        SCI_CREATEDOCUMENT,
        SCI_CREATELOADER,
        SCI_PRIVATELEXERCALL,
    };

    for (unsigned int msg : stateful_messages) {
        SQ_EXPECT(!scene_graph_message_is_known_read_only(msg));
        SQ_EXPECT(scene_graph_update_request(msg).needed);
    }
}

void test_tracked_scroll_width_reset_table()
{
    // Spot-check the scroll-width reset allow-list.
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_SETTEXT));
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_CLEARALL));
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_SETWRAPMODE));
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_STYLESETFONT));
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_SETMARGINWIDTHN));
    SQ_EXPECT(tracked_scroll_width_should_reset(SCI_SETDOCPOINTER));

    // Messages that must NOT trigger a scroll-width reset:
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_GETTEXT));
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_GOTOPOS));
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_SETSEL));
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_INSERTTEXT));
}

// Lock the invariant that the new scroll_width_reset field on
// scene_graph_update_request_info_t is a faithful mirror of
// tracked_scroll_width_should_reset(). Callers (ScintillaQuick_item::send)
// rely on this, so a future refactor that drifts the two must fail loudly.
void test_dispatch_struct_mirrors_scroll_width_reset_helper()
{
    constexpr unsigned int sweep_upper_bound = 4300;
    for (unsigned int msg = SCI_START; msg <= sweep_upper_bound; ++msg) {
        SQ_EXPECT(
            scene_graph_update_request(msg).scroll_width_reset
            == tracked_scroll_width_should_reset(msg));
    }
}

// Stage 1 refactor guard: the manual rule table must be behavior-
// equivalent to the pre-table switch classifier for every swept
// message and every caller-visible output field.
void test_stage1_rule_table_matches_legacy_switch_behavior()
{
    SQ_EXPECT(legacy_dispatch_rules_are_sorted());

    constexpr unsigned int sweep_upper_bound = 4300;
    for (unsigned int msg = SCI_START; msg <= sweep_upper_bound; ++msg) {
        const Legacy_dispatch_rule expected = legacy_dispatch_for_message(msg);
        const scene_graph_update_request_info_t actual_request = scene_graph_update_request(msg);

        expect_dispatch_field_equal(
            msg, "known_getter", scene_graph_message_is_known_getter(msg), expected.known_getter);
        expect_dispatch_field_equal(
            msg, "known_read_only", scene_graph_message_is_known_read_only(msg), expected.known_read_only);
        expect_dispatch_field_equal(msg, "needed", actual_request.needed, expected.needed);
        expect_dispatch_field_equal(
            msg, "static_content_dirty", actual_request.static_content_dirty, expected.static_content_dirty);
        expect_dispatch_field_equal(
            msg, "needs_style_sync", actual_request.needs_style_sync, expected.needs_style_sync);
        expect_dispatch_field_equal(msg, "scrolling", actual_request.scrolling, expected.scrolling);
        expect_dispatch_field_equal(
            msg, "scroll_width_reset", actual_request.scroll_width_reset, expected.scroll_width_reset);
        expect_dispatch_field_equal(
            msg,
            "tracked_scroll_width_should_reset",
            tracked_scroll_width_should_reset(msg),
            expected.scroll_width_reset);
    }
}
// Exhaustive sweep over Scintilla's entire public message range.
//
// The property enforced here is the single structural invariant of
// the dispatch table:
//
//   Every SCI_* message in the public range must either
//   (a) be classified as a known read-only query
//       (scene_graph_message_is_known_read_only() returns true and
//       scene_graph_update_request() returns `needed=false`), OR
//   (b) be classified as needing a scene-graph update
//       (scene_graph_update_request() returns `needed=true`).
//
// There must be no third case - no silent "not read-only AND not
// scheduling a resync" fall-through. That third case is the bug this
// test guards against: a default branch that returns an empty request,
// meaning messages absent from the allow-list silently skip the resync
// they need.
//
// This sweep does NOT police the specific combination of
// `static_content_dirty` / `needs_style_sync` / `scrolling` flags,
// because individual classifications in the table are permitted to be
// narrower than the conservative default when the table author knows
// exactly what a message touches. The conservative-default shape
// (`needed`+`static_content_dirty`+`needs_style_sync`) is asserted by
// test_unknown_messages_trigger_conservative_full_resync().
void test_full_scintilla_message_range_is_classified()
{
    // Scintilla's public message numbers start at SCI_START (= 2000)
    // and, at the time this test was written, top out around 4033.
    // We sweep a generous upper bound to catch both today's messages
    // and reasonably near-future additions, and we count every
    // classification so a regression is visible in the test log even
    // without reading every SQ_EXPECT failure.
    //
    // If Scintilla ever introduces a sentinel macro for the top of
    // the message range, prefer that over the hard-coded constant.
    constexpr unsigned int sweep_upper_bound = 4300;

    unsigned int classified_as_read_only = 0;
    unsigned int classified_as_resync    = 0;
    unsigned int silently_ignored        = 0;

    for (unsigned int msg = SCI_START; msg <= sweep_upper_bound; ++msg) {
        const bool read_only = scene_graph_message_is_known_read_only(msg);
        const scene_graph_update_request_info_t req = scene_graph_update_request(msg);

        if (read_only) {
            ++classified_as_read_only;
            SQ_EXPECT(!req.needed);
            SQ_EXPECT(!req.static_content_dirty);
            SQ_EXPECT(!req.needs_style_sync);
            SQ_EXPECT(!req.scrolling);
            continue;
        }

        if (req.needed) {
            ++classified_as_resync;
            continue;
        }

        // Neither a known read-only query, nor scheduling a resync:
        // this is exactly the historical silent-default bug.
        ++silently_ignored;
        std::fprintf(stderr,
            "FAIL: SCI_ message %u is neither known-read-only nor "
            "scheduling a resync. Add it to "
            "scene_graph_message_is_known_read_only() in "
            "src/core/scintillaquick_dispatch_table.h if it is a "
            "query, or make sure the conservative default still "
            "fires for it.\n",
            msg);
        SQ_EXPECT(false);
    }

    std::fprintf(stderr, "dispatch sweep: %u read-only, %u scheduled-resync, %u silently-ignored\n",
        classified_as_read_only, classified_as_resync, silently_ignored);

    SQ_EXPECT(silently_ignored == 0);
    // Sanity: we should be seeing at least some traffic on both
    // branches. If the sweep reports zero read-only classifications,
    // the allow-list has probably been wiped.
    SQ_EXPECT(classified_as_read_only > 0);
    SQ_EXPECT(classified_as_resync > 0);
}

} // namespace

int main()
{
    test_known_mutating_messages_request_update();
    test_specific_scroll_dispatch();
    test_caret_movement_dispatch_is_overlay_only();
    test_known_read_only_messages_take_fast_path();
    test_public_query_messages_take_fast_path();
    test_direct_query_families_take_fast_path();
    test_sync_quick_view_properties_path_is_recursion_safe();
    test_unknown_messages_trigger_conservative_full_resync();
    test_read_only_and_mutating_lists_are_disjoint();
    test_stateful_query_like_messages_are_not_read_only();
    test_tracked_scroll_width_reset_table();
    test_dispatch_struct_mirrors_scroll_width_reset_helper();
    test_stage1_rule_table_matches_legacy_switch_behavior();
    test_full_scintilla_message_range_is_classified();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d dispatch table test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::puts("dispatch table tests: all checks passed.");
    return EXIT_SUCCESS;
}
