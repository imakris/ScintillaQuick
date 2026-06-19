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

#include <cstddef>

namespace Scintilla::Internal
{

struct scene_graph_update_request_info_t
{
    bool needed               = false;
    bool static_content_dirty = false;
    bool needs_style_sync     = false;
    bool scrolling            = false;
    // Set by scene_graph_update_request() from the same rule entry as
    // the visual update classification so scroll-width behavior cannot
    // drift into a parallel switch.
    bool scroll_width_reset   = false;
};

namespace detail
{

enum class Dispatch_effect : unsigned char
{
    Read_only,
    Overlay,
    Static_content,
    Static_content_and_style,
    Scroll,
};

struct Message_rule
{
    unsigned int message;
    Dispatch_effect effect;
    bool scroll_width_reset;
};

// Single source of truth for classified messages. The array is sorted
// by numeric SCI_* id and looked up with binary search. Messages not
// listed here retain the conservative full-resync fallback.
//
// Overlay rules include caret/selection movement and caret/selection
// colour changes: they need a repaint, but they must not dirty static
// text/style content or force property/style synchronization.
inline constexpr Message_rule k_message_rules[] = {
    {SCI_INSERTTEXT, Dispatch_effect::Static_content_and_style, false},
    {SCI_CLEARALL, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETLENGTH, Dispatch_effect::Read_only, false},
    {SCI_GETCHARAT, Dispatch_effect::Read_only, false},
    {SCI_GETCURRENTPOS, Dispatch_effect::Read_only, false},
    {SCI_GETANCHOR, Dispatch_effect::Read_only, false},
    {SCI_GETSTYLEAT, Dispatch_effect::Read_only, false},
    {SCI_GETSTYLEDTEXT, Dispatch_effect::Read_only, false},
    {SCI_CANREDO, Dispatch_effect::Read_only, false},
    {SCI_MARKERLINEFROMHANDLE, Dispatch_effect::Read_only, false},
    {SCI_MARKERDELETEHANDLE, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETUNDOCOLLECTION, Dispatch_effect::Read_only, false},
    {SCI_GETVIEWWS, Dispatch_effect::Read_only, false},
    {SCI_POSITIONFROMPOINT, Dispatch_effect::Read_only, false},
    {SCI_POSITIONFROMPOINTCLOSE, Dispatch_effect::Read_only, false},
    {SCI_GOTOPOS, Dispatch_effect::Overlay, false},
    {SCI_SETANCHOR, Dispatch_effect::Overlay, false},
    {SCI_GETCURLINE, Dispatch_effect::Read_only, false},
    {SCI_GETENDSTYLED, Dispatch_effect::Read_only, false},
    {SCI_GETEOLMODE, Dispatch_effect::Read_only, false},
    {SCI_GETBUFFEREDDRAW, Dispatch_effect::Read_only, false},
    {SCI_SETTABWIDTH, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETSTYLEINDEXAT, Dispatch_effect::Read_only, false},
    {SCI_GETTEXTRANGEFULL, Dispatch_effect::Read_only, false},
    {SCI_MARKERDEFINE, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERSETFORE, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERSETBACK, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERADD, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERDELETE, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERDELETEALL, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERGET, Dispatch_effect::Read_only, false},
    {SCI_MARKERNEXT, Dispatch_effect::Read_only, false},
    {SCI_MARKERPREVIOUS, Dispatch_effect::Read_only, false},
    {SCI_MARKERDEFINEPIXMAP, Dispatch_effect::Static_content_and_style, false},
    {SCI_STYLECLEARALL, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLESETFORE, Dispatch_effect::Static_content_and_style, false},
    {SCI_STYLESETBACK, Dispatch_effect::Static_content_and_style, false},
    {SCI_STYLESETBOLD, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLESETITALIC, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLESETSIZE, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLESETFONT, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLESETEOLFILLED, Dispatch_effect::Static_content_and_style, false},
    {SCI_STYLESETUNDERLINE, Dispatch_effect::Static_content_and_style, false},
    {SCI_STYLESETSIZEFRACTIONAL, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLEGETSIZEFRACTIONAL, Dispatch_effect::Read_only, false},
    {SCI_STYLESETWEIGHT, Dispatch_effect::Static_content_and_style, true},
    {SCI_STYLEGETWEIGHT, Dispatch_effect::Read_only, false},
    {SCI_STYLESETCHARACTERSET, Dispatch_effect::Static_content_and_style, true},
    {SCI_SETSELFORE, Dispatch_effect::Overlay, false},
    {SCI_SETSELBACK, Dispatch_effect::Overlay, false},
    {SCI_SETCARETFORE, Dispatch_effect::Overlay, false},
    {SCI_STYLESETVISIBLE, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETCARETPERIOD, Dispatch_effect::Read_only, false},
    {SCI_INDICGETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_INDICGETFORE, Dispatch_effect::Read_only, false},
    {SCI_GETWHITESPACESIZE, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONHIDDEN, Dispatch_effect::Read_only, false},
#ifdef SCI_GETSTYLEBITS
    {SCI_GETSTYLEBITS, Dispatch_effect::Read_only, false},
#endif
    {SCI_GETLINESTATE, Dispatch_effect::Read_only, false},
    {SCI_GETMAXLINESTATE, Dispatch_effect::Read_only, false},
    {SCI_GETCARETLINEVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_SETCARETLINEVISIBLE, Dispatch_effect::Overlay, false},
    {SCI_GETCARETLINEBACK, Dispatch_effect::Read_only, false},
    {SCI_SETCARETLINEBACK, Dispatch_effect::Overlay, false},
    {SCI_AUTOCACTIVE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCPOSSTART, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETSEPARATOR, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETCANCELATSTART, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETCHOOSESINGLE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETIGNORECASE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETAUTOHIDE, Dispatch_effect::Read_only, false},
    {SCI_GETTABWIDTH, Dispatch_effect::Read_only, false},
    {SCI_GETINDENT, Dispatch_effect::Read_only, false},
    {SCI_SETUSETABS, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETUSETABS, Dispatch_effect::Read_only, false},
    {SCI_GETLINEINDENTATION, Dispatch_effect::Read_only, false},
    {SCI_GETLINEINDENTPOSITION, Dispatch_effect::Read_only, false},
    {SCI_GETCOLUMN, Dispatch_effect::Read_only, false},
    {SCI_GETHSCROLLBAR, Dispatch_effect::Read_only, false},
    {SCI_GETINDENTATIONGUIDES, Dispatch_effect::Read_only, false},
    {SCI_GETHIGHLIGHTGUIDE, Dispatch_effect::Read_only, false},
    {SCI_GETLINEENDPOSITION, Dispatch_effect::Read_only, false},
    {SCI_GETCODEPAGE, Dispatch_effect::Read_only, false},
    {SCI_GETCARETFORE, Dispatch_effect::Read_only, false},
    {SCI_GETREADONLY, Dispatch_effect::Read_only, false},
    {SCI_SETCURRENTPOS, Dispatch_effect::Overlay, false},
    {SCI_GETSELECTIONSTART, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONEND, Dispatch_effect::Read_only, false},
    {SCI_GETPRINTMAGNIFICATION, Dispatch_effect::Read_only, false},
    {SCI_GETPRINTCOLOURMODE, Dispatch_effect::Read_only, false},
    {SCI_FINDTEXT, Dispatch_effect::Read_only, false},
    {SCI_GETFIRSTVISIBLELINE, Dispatch_effect::Read_only, false},
    {SCI_GETLINE, Dispatch_effect::Read_only, false},
    {SCI_GETLINECOUNT, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINLEFT, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETMARGINLEFT, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINRIGHT, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETMARGINRIGHT, Dispatch_effect::Read_only, false},
    {SCI_GETMODIFY, Dispatch_effect::Read_only, false},
    {SCI_SETSEL, Dispatch_effect::Overlay, false},
    {SCI_GETSELTEXT, Dispatch_effect::Read_only, false},
    {SCI_GETTEXTRANGE, Dispatch_effect::Read_only, false},
    {SCI_POINTXFROMPOSITION, Dispatch_effect::Read_only, false},
    {SCI_POINTYFROMPOSITION, Dispatch_effect::Read_only, false},
    {SCI_LINEFROMPOSITION, Dispatch_effect::Read_only, false},
    {SCI_POSITIONFROMLINE, Dispatch_effect::Read_only, false},
    {SCI_REPLACESEL, Dispatch_effect::Static_content_and_style, false},
    {SCI_SETREADONLY, Dispatch_effect::Static_content_and_style, false},
    {SCI_CANPASTE, Dispatch_effect::Read_only, false},
    {SCI_CANUNDO, Dispatch_effect::Read_only, false},
    {SCI_SETTEXT, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETTEXT, Dispatch_effect::Read_only, false},
    {SCI_GETTEXTLENGTH, Dispatch_effect::Read_only, false},
    {SCI_GETDIRECTFUNCTION, Dispatch_effect::Read_only, false},
    {SCI_GETDIRECTPOINTER, Dispatch_effect::Read_only, false},
    {SCI_GETOVERTYPE, Dispatch_effect::Read_only, false},
    {SCI_SETCARETWIDTH, Dispatch_effect::Overlay, false},
    {SCI_GETCARETWIDTH, Dispatch_effect::Read_only, false},
    {SCI_GETTARGETSTART, Dispatch_effect::Read_only, false},
    {SCI_GETTARGETEND, Dispatch_effect::Read_only, false},
    {SCI_FINDTEXTFULL, Dispatch_effect::Read_only, false},
    {SCI_GETSEARCHFLAGS, Dispatch_effect::Read_only, false},
    {SCI_CALLTIPACTIVE, Dispatch_effect::Read_only, false},
    {SCI_CALLTIPPOSSTART, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETMAXWIDTH, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETMAXHEIGHT, Dispatch_effect::Read_only, false},
    {SCI_VISIBLEFROMDOCLINE, Dispatch_effect::Read_only, false},
    {SCI_DOCLINEFROMVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_GETFOLDLEVEL, Dispatch_effect::Read_only, false},
    {SCI_GETLASTCHILD, Dispatch_effect::Read_only, false},
    {SCI_GETFOLDPARENT, Dispatch_effect::Read_only, false},
    {SCI_GETLINEVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_GETFOLDEXPANDED, Dispatch_effect::Read_only, false},
    {SCI_WRAPCOUNT, Dispatch_effect::Read_only, false},
    {SCI_GETALLLINESVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINTYPEN, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETMARGINTYPEN, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINWIDTHN, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETMARGINWIDTHN, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINMASKN, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETMARGINMASKN, Dispatch_effect::Read_only, false},
    {SCI_SETMARGINSENSITIVEN, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETMARGINSENSITIVEN, Dispatch_effect::Read_only, false},
    {SCI_GETMARGINCURSORN, Dispatch_effect::Read_only, false},
    {SCI_GETMARGINBACKN, Dispatch_effect::Read_only, false},
    {SCI_GETMARGINS, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETCHECKMONOSPACED, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETINVISIBLEREPRESENTATION, Dispatch_effect::Read_only, false},
    {SCI_GETTABINDENTS, Dispatch_effect::Read_only, false},
    {SCI_GETBACKSPACEUNINDENTS, Dispatch_effect::Read_only, false},
    {SCI_GETMOUSEDWELLTIME, Dispatch_effect::Read_only, false},
    {SCI_WORDSTARTPOSITION, Dispatch_effect::Read_only, false},
    {SCI_WORDENDPOSITION, Dispatch_effect::Read_only, false},
    {SCI_SETWRAPMODE, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETWRAPMODE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETDROPRESTOFWORD, Dispatch_effect::Read_only, false},
    {SCI_GETLAYOUTCACHE, Dispatch_effect::Read_only, false},
    {SCI_SETSCROLLWIDTH, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETSCROLLWIDTH, Dispatch_effect::Read_only, false},
    {SCI_TEXTWIDTH, Dispatch_effect::Read_only, false},
    {SCI_GETENDATLASTLINE, Dispatch_effect::Read_only, false},
    {SCI_TEXTHEIGHT, Dispatch_effect::Read_only, false},
    {SCI_GETVSCROLLBAR, Dispatch_effect::Read_only, false},
    {SCI_APPENDTEXT, Dispatch_effect::Static_content_and_style, false},
#ifdef SCI_GETTWOPHASEDRAW
    {SCI_GETTWOPHASEDRAW, Dispatch_effect::Read_only, false},
#endif
    {SCI_AUTOCGETTYPESEPARATOR, Dispatch_effect::Read_only, false},
    {SCI_MARKERSETBACKSELECTED, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERENABLEHIGHLIGHT, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERSETFORETRANSLUCENT, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERSETBACKTRANSLUCENT, Dispatch_effect::Static_content_and_style, false},
    {SCI_MARKERSETBACKSELECTEDTRANSLUCENT, Dispatch_effect::Static_content_and_style, false},
    {SCI_LINEDOWN, Dispatch_effect::Overlay, false},
    {SCI_LINEDOWNEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEUP, Dispatch_effect::Overlay, false},
    {SCI_LINEUPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_CHARLEFT, Dispatch_effect::Overlay, false},
    {SCI_CHARLEFTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_CHARRIGHT, Dispatch_effect::Overlay, false},
    {SCI_CHARRIGHTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDLEFT, Dispatch_effect::Overlay, false},
    {SCI_WORDLEFTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDRIGHT, Dispatch_effect::Overlay, false},
    {SCI_WORDRIGHTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_HOME, Dispatch_effect::Overlay, false},
    {SCI_HOMEEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEEND, Dispatch_effect::Overlay, false},
    {SCI_LINEENDEXTEND, Dispatch_effect::Overlay, false},
    {SCI_DOCUMENTSTART, Dispatch_effect::Overlay, false},
    {SCI_DOCUMENTSTARTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_DOCUMENTEND, Dispatch_effect::Overlay, false},
    {SCI_DOCUMENTENDEXTEND, Dispatch_effect::Overlay, false},
    {SCI_PAGEUP, Dispatch_effect::Overlay, false},
    {SCI_PAGEUPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_PAGEDOWN, Dispatch_effect::Overlay, false},
    {SCI_PAGEDOWNEXTEND, Dispatch_effect::Overlay, false},
    {SCI_VCHOME, Dispatch_effect::Overlay, false},
    {SCI_VCHOMEEXTEND, Dispatch_effect::Overlay, false},
    {SCI_HOMEDISPLAY, Dispatch_effect::Overlay, false},
    {SCI_HOMEDISPLAYEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEENDDISPLAY, Dispatch_effect::Overlay, false},
    {SCI_LINEENDDISPLAYEXTEND, Dispatch_effect::Overlay, false},
    {SCI_HOMEWRAP, Dispatch_effect::Overlay, false},
    {SCI_LINELENGTH, Dispatch_effect::Read_only, false},
    {SCI_BRACEMATCH, Dispatch_effect::Read_only, false},
    {SCI_GETVIEWEOL, Dispatch_effect::Read_only, false},
    {SCI_GETDOCPOINTER, Dispatch_effect::Read_only, false},
    {SCI_SETDOCPOINTER, Dispatch_effect::Static_content_and_style, true},
    {SCI_GETEDGECOLUMN, Dispatch_effect::Read_only, false},
    {SCI_GETEDGEMODE, Dispatch_effect::Read_only, false},
    {SCI_GETEDGECOLOUR, Dispatch_effect::Read_only, false},
    {SCI_BRACEMATCHNEXT, Dispatch_effect::Read_only, false},
    {SCI_LINESONSCREEN, Dispatch_effect::Read_only, false},
    {SCI_SELECTIONISRECTANGLE, Dispatch_effect::Read_only, false},
    {SCI_GETZOOM, Dispatch_effect::Read_only, false},
    {SCI_GETMODEVENTMASK, Dispatch_effect::Read_only, false},
    {SCI_GETDOCUMENTOPTIONS, Dispatch_effect::Read_only, false},
    {SCI_GETFOCUS, Dispatch_effect::Read_only, false},
    {SCI_GETSTATUS, Dispatch_effect::Read_only, false},
    {SCI_GETMOUSEDOWNCAPTURES, Dispatch_effect::Read_only, false},
    {SCI_GETCURSOR, Dispatch_effect::Read_only, false},
    {SCI_GETCONTROLCHARSYMBOL, Dispatch_effect::Read_only, false},
    {SCI_WORDPARTLEFT, Dispatch_effect::Overlay, false},
    {SCI_WORDPARTLEFTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDPARTRIGHT, Dispatch_effect::Overlay, false},
    {SCI_WORDPARTRIGHTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_SETXOFFSET, Dispatch_effect::Static_content, false},
    {SCI_GETXOFFSET, Dispatch_effect::Read_only, false},
    {SCI_GETPRINTWRAPMODE, Dispatch_effect::Read_only, false},
    {SCI_POSITIONBEFORE, Dispatch_effect::Read_only, false},
    {SCI_POSITIONAFTER, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONMODE, Dispatch_effect::Read_only, false},
    {SCI_GETLINESELSTARTPOSITION, Dispatch_effect::Read_only, false},
    {SCI_GETLINESELENDPOSITION, Dispatch_effect::Read_only, false},
    {SCI_LINEDOWNRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEUPRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_CHARLEFTRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_CHARRIGHTRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_HOMERECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_VCHOMERECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEENDRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_PAGEUPRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_PAGEDOWNRECTEXTEND, Dispatch_effect::Overlay, false},
    {SCI_STUTTEREDPAGEUP, Dispatch_effect::Overlay, false},
    {SCI_STUTTEREDPAGEUPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_STUTTEREDPAGEDOWN, Dispatch_effect::Overlay, false},
    {SCI_STUTTEREDPAGEDOWNEXTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDLEFTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDLEFTENDEXTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDRIGHTEND, Dispatch_effect::Overlay, false},
    {SCI_WORDRIGHTENDEXTEND, Dispatch_effect::Overlay, false},
    {SCI_AUTOCGETCURRENT, Dispatch_effect::Read_only, false},
    {SCI_TARGETASUTF8, Dispatch_effect::Read_only, false},
    {SCI_ENCODEDFROMUTF8, Dispatch_effect::Read_only, false},
    {SCI_HOMEWRAPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_LINEENDWRAP, Dispatch_effect::Overlay, false},
    {SCI_LINEENDWRAPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_VCHOMEWRAP, Dispatch_effect::Overlay, false},
    {SCI_VCHOMEWRAPEXTEND, Dispatch_effect::Overlay, false},
    {SCI_FINDCOLUMN, Dispatch_effect::Read_only, false},
    {SCI_GETCARETSTICKY, Dispatch_effect::Read_only, false},
    {SCI_GETWRAPVISUALFLAGS, Dispatch_effect::Read_only, false},
    {SCI_GETWRAPVISUALFLAGSLOCATION, Dispatch_effect::Read_only, false},
    {SCI_GETWRAPSTARTINDENT, Dispatch_effect::Read_only, false},
    {SCI_MARKERADDSET, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETPASTECONVERTENDINGS, Dispatch_effect::Read_only, false},
    {SCI_GETCARETLINEBACKALPHA, Dispatch_effect::Read_only, false},
    {SCI_GETWRAPINDENTMODE, Dispatch_effect::Read_only, false},
    {SCI_GETSELALPHA, Dispatch_effect::Read_only, false},
    {SCI_GETSELEOLFILLED, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETFORE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETBACK, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETBOLD, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETITALIC, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETSIZE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETFONT, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETEOLFILLED, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETUNDERLINE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETCASE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETCHARACTERSET, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETCHANGEABLE, Dispatch_effect::Read_only, false},
    {SCI_STYLEGETHOTSPOT, Dispatch_effect::Read_only, false},
    {SCI_GETHOTSPOTACTIVEFORE, Dispatch_effect::Read_only, false},
    {SCI_GETHOTSPOTACTIVEBACK, Dispatch_effect::Read_only, false},
    {SCI_GETHOTSPOTACTIVEUNDERLINE, Dispatch_effect::Read_only, false},
    {SCI_GETHOTSPOTSINGLELINE, Dispatch_effect::Read_only, false},
    {SCI_GETINDICATORCURRENT, Dispatch_effect::Read_only, false},
    {SCI_GETINDICATORVALUE, Dispatch_effect::Read_only, false},
    {SCI_INDICATORALLONFOR, Dispatch_effect::Read_only, false},
    {SCI_INDICATORVALUEAT, Dispatch_effect::Read_only, false},
    {SCI_INDICATORSTART, Dispatch_effect::Read_only, false},
    {SCI_INDICATOREND, Dispatch_effect::Read_only, false},
    {SCI_INDICGETUNDER, Dispatch_effect::Read_only, false},
    {SCI_GETCARETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_GETPOSITIONCACHE, Dispatch_effect::Read_only, false},
    {SCI_GETSCROLLWIDTHTRACKING, Dispatch_effect::Read_only, false},
    {SCI_GETCHARACTERPOINTER, Dispatch_effect::Read_only, false},
#ifdef SCI_GETKEYSUNICODE
    {SCI_GETKEYSUNICODE, Dispatch_effect::Read_only, false},
#endif
    {SCI_INDICGETALPHA, Dispatch_effect::Read_only, false},
    {SCI_GETEXTRAASCENT, Dispatch_effect::Read_only, false},
    {SCI_GETEXTRADESCENT, Dispatch_effect::Read_only, false},
    {SCI_MARKERSYMBOLDEFINED, Dispatch_effect::Read_only, false},
    {SCI_MARGINGETTEXT, Dispatch_effect::Read_only, false},
    {SCI_MARGINGETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_MARGINGETSTYLES, Dispatch_effect::Read_only, false},
    {SCI_MARGINGETSTYLEOFFSET, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETTEXT, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETSTYLES, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETLINES, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_ANNOTATIONGETSTYLEOFFSET, Dispatch_effect::Read_only, false},
    {SCI_SETEMPTYSELECTION, Dispatch_effect::Overlay, false},
    {SCI_GETMARGINOPTIONS, Dispatch_effect::Read_only, false},
    {SCI_INDICGETOUTLINEALPHA, Dispatch_effect::Read_only, false},
    {SCI_CHARPOSITIONFROMPOINT, Dispatch_effect::Read_only, false},
    {SCI_CHARPOSITIONFROMPOINTCLOSE, Dispatch_effect::Read_only, false},
    {SCI_GETMULTIPLESELECTION, Dispatch_effect::Read_only, false},
    {SCI_GETADDITIONALSELECTIONTYPING, Dispatch_effect::Read_only, false},
    {SCI_GETADDITIONALCARETSBLINK, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONS, Dispatch_effect::Read_only, false},
    {SCI_GETMAINSELECTION, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNCARET, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNANCHOR, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNCARETVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNANCHORVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNSTART, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNEND, Dispatch_effect::Read_only, false},
    {SCI_GETRECTANGULARSELECTIONCARET, Dispatch_effect::Read_only, false},
    {SCI_GETRECTANGULARSELECTIONANCHOR, Dispatch_effect::Read_only, false},
    {SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETVIRTUALSPACEOPTIONS, Dispatch_effect::Read_only, false},
    {SCI_GETRECTANGULARSELECTIONMODIFIER, Dispatch_effect::Read_only, false},
    {SCI_GETADDITIONALSELALPHA, Dispatch_effect::Read_only, false},
    {SCI_GETADDITIONALCARETFORE, Dispatch_effect::Read_only, false},
    {SCI_GETADDITIONALCARETSVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETCURRENTTEXT, Dispatch_effect::Read_only, false},
    {SCI_GETFONTQUALITY, Dispatch_effect::Read_only, false},
    {SCI_SETFIRSTVISIBLELINE, Dispatch_effect::Scroll, false},
    {SCI_GETMULTIPASTE, Dispatch_effect::Read_only, false},
    {SCI_GETTAG, Dispatch_effect::Read_only, false},
    {SCI_CONTRACTEDFOLDNEXT, Dispatch_effect::Read_only, false},
    {SCI_GETIDENTIFIER, Dispatch_effect::Read_only, false},
    {SCI_MARKERDEFINERGBAIMAGE, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETTECHNOLOGY, Dispatch_effect::Read_only, false},
    {SCI_COUNTCHARACTERS, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETCASEINSENSITIVEBEHAVIOUR, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETMULTI, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETOPTIONS, Dispatch_effect::Read_only, false},
    {SCI_GETRANGEPOINTER, Dispatch_effect::Read_only, false},
    {SCI_GETGAPPOSITION, Dispatch_effect::Read_only, false},
    {SCI_DELETERANGE, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETWORDCHARS, Dispatch_effect::Read_only, false},
    {SCI_GETWHITESPACECHARS, Dispatch_effect::Read_only, false},
    {SCI_GETPUNCTUATIONCHARS, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONEMPTY, Dispatch_effect::Read_only, false},
    {SCI_VCHOMEDISPLAY, Dispatch_effect::Overlay, false},
    {SCI_VCHOMEDISPLAYEXTEND, Dispatch_effect::Overlay, false},
    {SCI_GETCARETLINEVISIBLEALWAYS, Dispatch_effect::Read_only, false},
    {SCI_GETLINEENDTYPESALLOWED, Dispatch_effect::Read_only, false},
    {SCI_GETLINEENDTYPESACTIVE, Dispatch_effect::Read_only, false},
    {SCI_AUTOCGETORDER, Dispatch_effect::Read_only, false},
    {SCI_GETAUTOMATICFOLD, Dispatch_effect::Read_only, false},
    {SCI_GETREPRESENTATION, Dispatch_effect::Read_only, false},
    {SCI_GETMOUSESELECTIONRECTANGULARSWITCH, Dispatch_effect::Read_only, false},
    {SCI_POSITIONRELATIVE, Dispatch_effect::Read_only, false},
    {SCI_GETPHASESDRAW, Dispatch_effect::Read_only, false},
    {SCI_GETNEXTTABSTOP, Dispatch_effect::Read_only, false},
    {SCI_GETIMEINTERACTION, Dispatch_effect::Read_only, false},
    {SCI_INDICGETHOVERSTYLE, Dispatch_effect::Read_only, false},
    {SCI_INDICGETHOVERFORE, Dispatch_effect::Read_only, false},
    {SCI_INDICGETFLAGS, Dispatch_effect::Read_only, false},
    {SCI_GETTARGETTEXT, Dispatch_effect::Read_only, false},
    {SCI_ISRANGEWORD, Dispatch_effect::Read_only, false},
    {SCI_GETIDLESTYLING, Dispatch_effect::Read_only, false},
    {SCI_GETMOUSEWHEELCAPTURES, Dispatch_effect::Read_only, false},
    {SCI_GETTABDRAWMODE, Dispatch_effect::Read_only, false},
    {SCI_GETACCESSIBILITY, Dispatch_effect::Read_only, false},
    {SCI_GETCARETLINEFRAME, Dispatch_effect::Read_only, false},
    {SCI_GETMOVEEXTENDSSELECTION, Dispatch_effect::Read_only, false},
    {SCI_FOLDDISPLAYTEXTGETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_GETBIDIRECTIONAL, Dispatch_effect::Read_only, false},
    {SCI_SETBIDIRECTIONAL, Dispatch_effect::Static_content_and_style, false},
    {SCI_GETLINECHARACTERINDEX, Dispatch_effect::Read_only, false},
    {SCI_LINEFROMINDEXPOSITION, Dispatch_effect::Read_only, false},
    {SCI_INDEXPOSITIONFROMLINE, Dispatch_effect::Read_only, false},
    {SCI_COUNTCODEUNITS, Dispatch_effect::Read_only, false},
    {SCI_POSITIONRELATIVECODEUNITS, Dispatch_effect::Read_only, false},
    {SCI_GETCOMMANDEVENTS, Dispatch_effect::Read_only, false},
    {SCI_GETCHARACTERCATEGORYOPTIMIZATION, Dispatch_effect::Read_only, false},
    {SCI_GETDEFAULTFOLDDISPLAYTEXT, Dispatch_effect::Read_only, false},
    {SCI_GETTABMINIMUMWIDTH, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNSTARTVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONNENDVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETTARGETSTARTVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_GETTARGETENDVIRTUALSPACE, Dispatch_effect::Read_only, false},
    {SCI_MARKERHANDLEFROMLINE, Dispatch_effect::Read_only, false},
    {SCI_MARKERNUMBERFROMLINE, Dispatch_effect::Read_only, false},
    {SCI_MARKERGETLAYER, Dispatch_effect::Read_only, false},
    {SCI_EOLANNOTATIONGETTEXT, Dispatch_effect::Read_only, false},
    {SCI_EOLANNOTATIONGETSTYLE, Dispatch_effect::Read_only, false},
    {SCI_EOLANNOTATIONGETVISIBLE, Dispatch_effect::Read_only, false},
    {SCI_EOLANNOTATIONGETSTYLEOFFSET, Dispatch_effect::Read_only, false},
    {SCI_GETMULTIEDGECOLUMN, Dispatch_effect::Read_only, false},
    {SCI_SUPPORTSFEATURE, Dispatch_effect::Read_only, false},
    {SCI_INDICGETSTROKEWIDTH, Dispatch_effect::Read_only, false},
    {SCI_GETELEMENTCOLOUR, Dispatch_effect::Read_only, false},
    {SCI_GETELEMENTISSET, Dispatch_effect::Read_only, false},
    {SCI_GETELEMENTALLOWSTRANSLUCENT, Dispatch_effect::Read_only, false},
    {SCI_GETELEMENTBASECOLOUR, Dispatch_effect::Read_only, false},
    {SCI_GETFONTLOCALE, Dispatch_effect::Read_only, false},
    {SCI_GETSELECTIONLAYER, Dispatch_effect::Read_only, false},
    {SCI_GETCARETLINELAYER, Dispatch_effect::Read_only, false},
    {SCI_GETREPRESENTATIONAPPEARANCE, Dispatch_effect::Read_only, false},
    {SCI_GETREPRESENTATIONCOLOUR, Dispatch_effect::Read_only, false},
    {SCI_GETDIRECTSTATUSFUNCTION, Dispatch_effect::Read_only, false},
    {SCI_GETCARETLINEHIGHLIGHTSUBLINE, Dispatch_effect::Read_only, false},
    {SCI_GETLAYOUTTHREADS, Dispatch_effect::Read_only, false},
    {SCI_GETSTYLEDTEXTFULL, Dispatch_effect::Read_only, false},
    {SCI_GETCHANGEHISTORY, Dispatch_effect::Read_only, false},
    {SCI_GETLEXER, Dispatch_effect::Read_only, false},
    {SCI_GETPROPERTY, Dispatch_effect::Read_only, false},
    {SCI_GETPROPERTYEXPANDED, Dispatch_effect::Read_only, false},
    {SCI_GETPROPERTYINT, Dispatch_effect::Read_only, false},
#ifdef SCI_GETSTYLEBITSNEEDED
    {SCI_GETSTYLEBITSNEEDED, Dispatch_effect::Read_only, false},
#endif
    {SCI_GETLEXERLANGUAGE, Dispatch_effect::Read_only, false},
    {SCI_PROPERTYNAMES, Dispatch_effect::Read_only, false},
    {SCI_PROPERTYTYPE, Dispatch_effect::Read_only, false},
    {SCI_DESCRIBEPROPERTY, Dispatch_effect::Read_only, false},
    {SCI_DESCRIBEKEYWORDSETS, Dispatch_effect::Read_only, false},
    {SCI_GETLINEENDTYPESSUPPORTED, Dispatch_effect::Read_only, false},
    {SCI_GETSUBSTYLESSTART, Dispatch_effect::Read_only, false},
    {SCI_GETSUBSTYLESLENGTH, Dispatch_effect::Read_only, false},
    {SCI_DISTANCETOSECONDARYSTYLES, Dispatch_effect::Read_only, false},
    {SCI_GETSUBSTYLEBASES, Dispatch_effect::Read_only, false},
    {SCI_GETSTYLEFROMSUBSTYLE, Dispatch_effect::Read_only, false},
    {SCI_GETPRIMARYSTYLEFROMSTYLE, Dispatch_effect::Read_only, false},
    {SCI_GETNAMEDSTYLES, Dispatch_effect::Read_only, false},
    {SCI_NAMEOFSTYLE, Dispatch_effect::Read_only, false},
    {SCI_TAGSOFSTYLE, Dispatch_effect::Read_only, false},
    {SCI_DESCRIPTIONOFSTYLE, Dispatch_effect::Read_only, false},
};

constexpr bool message_rules_are_sorted()
{
    for (std::size_t i = 1; i < sizeof(k_message_rules) / sizeof(k_message_rules[0]); ++i) {
        if (k_message_rules[i - 1].message >= k_message_rules[i].message) {
            return false;
        }
    }

    return true;
}

static_assert(message_rules_are_sorted(), "ScintillaQuick dispatch rules must be sorted and unique");

inline const Message_rule* find_message_rule(unsigned int message)
{
    std::size_t first = 0;
    std::size_t count = sizeof(k_message_rules) / sizeof(k_message_rules[0]);

    while (count > 0) {
        const std::size_t step = count / 2;
        const std::size_t index = first + step;
        const Message_rule& rule = k_message_rules[index];

        if (rule.message < message) {
            first = index + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    if (first < sizeof(k_message_rules) / sizeof(k_message_rules[0])
        && k_message_rules[first].message == message) {
        return &k_message_rules[first];
    }

    return nullptr;
}

constexpr scene_graph_update_request_info_t info_for_effect(Dispatch_effect effect)
{
    switch (effect) {
        case Dispatch_effect::Read_only:
            return {};
        case Dispatch_effect::Overlay:
            return {true, false, false, false};
        case Dispatch_effect::Static_content:
            return {true, true, false, false};
        case Dispatch_effect::Static_content_and_style:
            return {true, true, true, false};
        case Dispatch_effect::Scroll:
            return {true, true, true, true};
    }

    return {true, true, true, false};
}

inline scene_graph_update_request_info_t scene_graph_update_request_classify(unsigned int i_message)
{
    if (const Message_rule* rule = find_message_rule(i_message)) {
        return info_for_effect(rule->effect);
    }

    // Unknown message: default to a conservative full resync. Repainting
    // too often is a performance bug; repainting too rarely is a visual
    // correctness bug, and new Scintilla messages that mutate state would
    // otherwise be silently skipped here.
    return {true, true, true, false};
}

} // namespace detail

// Known read-only / query-like Scintilla messages that are safe to treat
// as non-visual. Messages listed with Dispatch_effect::Read_only take
// the fast path and skip scene-graph resync.
//
// CRITICAL invariant: every SCI_* message that ScintillaQuick_item or its
// helper getters call INTERNALLY via `send()` must appear in the table as
// read-only if it is a pure query. If it does not, the conservative default
// in `scene_graph_update_request()` will trigger a full resync, which can
// re-enter `syncQuickViewProperties()` and recurse.
inline bool scene_graph_message_is_known_read_only(unsigned int i_message)
{
    const detail::Message_rule* rule = detail::find_message_rule(i_message);
    return rule && rule->effect == detail::Dispatch_effect::Read_only;
}

// Single dispatch entry point. The update classification and scroll-width
// reset flag now come from one Message_rule entry instead of parallel
// switches.
inline scene_graph_update_request_info_t scene_graph_update_request(unsigned int i_message)
{
    scene_graph_update_request_info_t info = detail::scene_graph_update_request_classify(i_message);

    if (const detail::Message_rule* rule = detail::find_message_rule(i_message)) {
        info.scroll_width_reset = rule->scroll_width_reset;
    }

    return info;
}

} // namespace Scintilla::Internal

#endif // SCINTILLAQUICK_DISPATCH_TABLE_H
