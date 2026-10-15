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

#include <algorithm>
#include <cstddef>
#include <iterator>

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
    READ_ONLY,
    OVERLAY,
    STATIC_CONTENT,
    STATIC_CONTENT_AND_STYLE,
    SCROLL,
};

struct Message_rule
{
    unsigned int    message;
    Dispatch_effect effect;
    bool            scroll_width_reset;
};

// Single source of truth for classified messages. The array is sorted
// by numeric SCI_* id and looked up with binary search. Messages not
// listed here retain the conservative full-resync fallback.
//
// Overlay rules include caret/selection movement and caret/selection
// colour changes: they need a repaint, but they must not dirty static
// text/style content or force property/style synchronization.
inline constexpr Message_rule k_message_rules[] = {
    {SCI_INSERTTEXT,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_CLEARALL,                                  Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETLENGTH,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCHARAT,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCURRENTPOS,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETANCHOR,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSTYLEAT,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_SELECTALL,                                 Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSTYLEDTEXT,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_CANREDO,                                   Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERLINEFROMHANDLE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERDELETEHANDLE,                        Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOCOLLECTION,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETVIEWWS,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONFROMPOINT,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONFROMPOINTCLOSE,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GOTOPOS,                                   Dispatch_effect::OVERLAY,                  false},
    {SCI_SETANCHOR,                                 Dispatch_effect::OVERLAY,                  false},
    {SCI_GETCURLINE,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETENDSTYLED,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETEOLMODE,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETBUFFEREDDRAW,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_SETTABWIDTH,                               Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETSTYLEINDEXAT,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTEXTRANGEFULL,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERDEFINE,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERSETFORE,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERSETBACK,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERADD,                                 Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERDELETE,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERDELETEALL,                           Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERGET,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERNEXT,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERPREVIOUS,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERDEFINEPIXMAP,                        Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_STYLECLEARALL,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLESETFORE,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_STYLESETBACK,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_STYLESETBOLD,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLESETITALIC,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLESETSIZE,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLESETFONT,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLESETEOLFILLED,                         Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_STYLESETUNDERLINE,                         Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_STYLESETSIZEFRACTIONAL,                    Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLEGETSIZEFRACTIONAL,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLESETWEIGHT,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLEGETWEIGHT,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLESETCHARACTERSET,                      Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_SETSELFORE,                                Dispatch_effect::OVERLAY,                  false},
    {SCI_SETSELBACK,                                Dispatch_effect::OVERLAY,                  false},
    {SCI_SETCARETFORE,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_STYLESETVISIBLE,                           Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETCARETPERIOD,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_BEGINUNDOACTION,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_ENDUNDOACTION,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETSTYLE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETFORE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWHITESPACESIZE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONHIDDEN,                        Dispatch_effect::READ_ONLY,                false},
#ifdef SCI_GETSTYLEBITS
    {SCI_GETSTYLEBITS,                              Dispatch_effect::READ_ONLY,                false},
#endif
    {SCI_GETLINESTATE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMAXLINESTATE,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETLINEVISIBLE,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_SETCARETLINEVISIBLE,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_GETCARETLINEBACK,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETCARETLINEBACK,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_AUTOCACTIVE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCPOSSTART,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETSEPARATOR,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCSETSTYLE,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_AUTOCGETCANCELATSTART,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETCHOOSESINGLE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETIGNORECASE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETAUTOHIDE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETSTYLE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTABWIDTH,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETINDENT,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUSETABS,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETUSETABS,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEINDENTATION,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEINDENTPOSITION,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCOLUMN,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_SETHSCROLLBAR,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETHSCROLLBAR,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETINDENTATIONGUIDES,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETHIGHLIGHTGUIDE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEENDPOSITION,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCODEPAGE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETFORE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETREADONLY,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_SETCURRENTPOS,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSELECTIONSTART,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONEND,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPRINTMAGNIFICATION,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPRINTCOLOURMODE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_FINDTEXT,                                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFIRSTVISIBLELINE,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINE,                                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINECOUNT,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINLEFT,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETMARGINLEFT,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINRIGHT,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETMARGINRIGHT,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMODIFY,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_SETSEL,                                    Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSELTEXT,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTEXTRANGE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_POINTXFROMPOSITION,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_POINTYFROMPOSITION,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_LINEFROMPOSITION,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONFROMLINE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SCROLLCARET,                               Dispatch_effect::SCROLL,                   false},
    {SCI_REPLACESEL,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_SETREADONLY,                               Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_CANPASTE,                                  Dispatch_effect::READ_ONLY,                false},
    {SCI_CANUNDO,                                   Dispatch_effect::READ_ONLY,                false},
    {SCI_SETTEXT,                                   Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETTEXT,                                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTEXTLENGTH,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDIRECTFUNCTION,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDIRECTPOINTER,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETOVERTYPE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_SETCARETWIDTH,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_GETCARETWIDTH,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTARGETSTART,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTARGETEND,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_REPLACETARGET,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_REPLACETARGETRE,                           Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_FINDTEXTFULL,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_SEARCHINTARGET,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_SETSEARCHFLAGS,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSEARCHFLAGS,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_CALLTIPACTIVE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_CALLTIPPOSSTART,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETMAXWIDTH,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETMAXHEIGHT,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_VISIBLEFROMDOCLINE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_DOCLINEFROMVISIBLE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFOLDLEVEL,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLASTCHILD,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFOLDPARENT,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEVISIBLE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFOLDEXPANDED,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_WRAPCOUNT,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETALLLINESVISIBLE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINTYPEN,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETMARGINTYPEN,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINWIDTHN,                           Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETMARGINWIDTHN,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINMASKN,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETMARGINMASKN,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMARGINSENSITIVEN,                       Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETMARGINSENSITIVEN,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMARGINCURSORN,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMARGINBACKN,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMARGINS,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETCHECKMONOSPACED,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETINVISIBLEREPRESENTATION,           Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLESETSTRETCH,                           Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_STYLEGETSTRETCH,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTABINDENTS,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETBACKSPACEUNINDENTS,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMOUSEDWELLTIME,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_WORDSTARTPOSITION,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_WORDENDPOSITION,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_SETWRAPMODE,                               Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETWRAPMODE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETDROPRESTOFWORD,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLAYOUTCACHE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_SETSCROLLWIDTH,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETSCROLLWIDTH,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_TEXTWIDTH,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETENDATLASTLINE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_TEXTHEIGHT,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_SETVSCROLLBAR,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETVSCROLLBAR,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_APPENDTEXT,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
#ifdef SCI_GETTWOPHASEDRAW
    {SCI_GETTWOPHASEDRAW,                           Dispatch_effect::READ_ONLY,                false},
#endif
    {SCI_AUTOCGETTYPESEPARATOR,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERSETBACKSELECTED,                     Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERENABLEHIGHLIGHT,                     Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERSETFORETRANSLUCENT,                  Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERSETBACKTRANSLUCENT,                  Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_MARKERSETBACKSELECTEDTRANSLUCENT,          Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_LINEDOWN,                                  Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEDOWNEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEUP,                                    Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEUPEXTEND,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARLEFT,                                  Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARLEFTEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARRIGHT,                                 Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARRIGHTEXTEND,                           Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDLEFT,                                  Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDLEFTEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDRIGHT,                                 Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDRIGHTEXTEND,                           Dispatch_effect::OVERLAY,                  false},
    {SCI_HOME,                                      Dispatch_effect::OVERLAY,                  false},
    {SCI_HOMEEXTEND,                                Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEEND,                                   Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDEXTEND,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_DOCUMENTSTART,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_DOCUMENTSTARTEXTEND,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_DOCUMENTEND,                               Dispatch_effect::OVERLAY,                  false},
    {SCI_DOCUMENTENDEXTEND,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEUP,                                    Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEUPEXTEND,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEDOWN,                                  Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEDOWNEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOME,                                    Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOMEEXTEND,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_HOMEDISPLAY,                               Dispatch_effect::OVERLAY,                  false},
    {SCI_HOMEDISPLAYEXTEND,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDDISPLAY,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDDISPLAYEXTEND,                      Dispatch_effect::OVERLAY,                  false},
    {SCI_HOMEWRAP,                                  Dispatch_effect::OVERLAY,                  false},
    {SCI_LINELENGTH,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_BRACEMATCH,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETVIEWEOL,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDOCPOINTER,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_SETDOCPOINTER,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, true },
    {SCI_GETEDGECOLUMN,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETEDGEMODE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETEDGECOLOUR,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_BRACEMATCHNEXT,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_LINESONSCREEN,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_SELECTIONISRECTANGLE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETZOOM,                                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMODEVENTMASK,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDOCUMENTOPTIONS,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFOCUS,                                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSTATUS,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMOUSEDOWNCAPTURES,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCURSOR,                                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCONTROLCHARSYMBOL,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_WORDPARTLEFT,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDPARTLEFTEXTEND,                        Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDPARTRIGHT,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDPARTRIGHTEXTEND,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_SETXOFFSET,                                Dispatch_effect::STATIC_CONTENT,           false},
    {SCI_GETXOFFSET,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPRINTWRAPMODE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONBEFORE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONAFTER,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONMODE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINESELSTARTPOSITION,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINESELENDPOSITION,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_LINEDOWNRECTEXTEND,                        Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEUPRECTEXTEND,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARLEFTRECTEXTEND,                        Dispatch_effect::OVERLAY,                  false},
    {SCI_CHARRIGHTRECTEXTEND,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_HOMERECTEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOMERECTEXTEND,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDRECTEXTEND,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEUPRECTEXTEND,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_PAGEDOWNRECTEXTEND,                        Dispatch_effect::OVERLAY,                  false},
    {SCI_STUTTEREDPAGEUP,                           Dispatch_effect::OVERLAY,                  false},
    {SCI_STUTTEREDPAGEUPEXTEND,                     Dispatch_effect::OVERLAY,                  false},
    {SCI_STUTTEREDPAGEDOWN,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_STUTTEREDPAGEDOWNEXTEND,                   Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDLEFTEND,                               Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDLEFTENDEXTEND,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDRIGHTEND,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_WORDRIGHTENDEXTEND,                        Dispatch_effect::OVERLAY,                  false},
    {SCI_AUTOCGETCURRENT,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_TARGETASUTF8,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_ENCODEDFROMUTF8,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_HOMEWRAPEXTEND,                            Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDWRAP,                               Dispatch_effect::OVERLAY,                  false},
    {SCI_LINEENDWRAPEXTEND,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOMEWRAP,                                Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOMEWRAPEXTEND,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_FINDCOLUMN,                                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETSTICKY,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWRAPVISUALFLAGS,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWRAPVISUALFLAGSLOCATION,                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWRAPSTARTINDENT,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERADDSET,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETPASTECONVERTENDINGS,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETLINEBACKALPHA,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWRAPINDENTMODE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_SELECTIONFROMPOINT,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELALPHA,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELEOLFILLED,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETFORE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETBACK,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETBOLD,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETITALIC,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETSIZE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETFONT,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETEOLFILLED,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETUNDERLINE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETCASE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETCHARACTERSET,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETVISIBLE,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETCHANGEABLE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_STYLEGETHOTSPOT,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETHOTSPOTACTIVEFORE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETHOTSPOTACTIVEBACK,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETHOTSPOTACTIVEUNDERLINE,                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETHOTSPOTSINGLELINE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETINDICATORCURRENT,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETINDICATORVALUE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICATORALLONFOR,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICATORVALUEAT,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICATORSTART,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICATOREND,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETUNDER,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETSTYLE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPOSITIONCACHE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETSCROLLWIDTHTRACKING,                    Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSCROLLWIDTHTRACKING,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCHARACTERPOINTER,                       Dispatch_effect::READ_ONLY,                false},
#ifdef SCI_GETKEYSUNICODE
    {SCI_GETKEYSUNICODE,                            Dispatch_effect::READ_ONLY,                false},
#endif
    {SCI_INDICGETALPHA,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETEXTRAASCENT,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETEXTRADESCENT,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERSYMBOLDEFINED,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_MARGINGETTEXT,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_MARGINGETSTYLE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_MARGINGETSTYLES,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_MARGINGETSTYLEOFFSET,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETTEXT,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETSTYLE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETSTYLES,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETLINES,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETVISIBLE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_ANNOTATIONGETSTYLEOFFSET,                  Dispatch_effect::READ_ONLY,                false},
    {SCI_SETEMPTYSELECTION,                         Dispatch_effect::OVERLAY,                  false},
    {SCI_GETMARGINOPTIONS,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETOUTLINEALPHA,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_CHARPOSITIONFROMPOINT,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_CHARPOSITIONFROMPOINTCLOSE,                Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMULTIPLESELECTION,                      Dispatch_effect::OVERLAY,                  false},
    {SCI_GETMULTIPLESELECTION,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETADDITIONALSELECTIONTYPING,              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETADDITIONALCARETSBLINK,                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONS,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_CLEARSELECTIONS,                           Dispatch_effect::OVERLAY,                  false},
    {SCI_ADDSELECTION,                              Dispatch_effect::OVERLAY,                  false},
    {SCI_SETMAINSELECTION,                          Dispatch_effect::OVERLAY,                  false},
    {SCI_GETMAINSELECTION,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNCARET,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNANCHOR,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNCARETVIRTUALSPACE,            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNANCHORVIRTUALSPACE,           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNSTART,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNEND,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRECTANGULARSELECTIONCARET,              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRECTANGULARSELECTIONANCHOR,             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE,  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE, Dispatch_effect::READ_ONLY,                false},
    {SCI_GETVIRTUALSPACEOPTIONS,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRECTANGULARSELECTIONMODIFIER,           Dispatch_effect::READ_ONLY,                false},
    {SCI_SETADDITIONALSELFORE,                      Dispatch_effect::OVERLAY,                  false},
    {SCI_SETADDITIONALSELBACK,                      Dispatch_effect::OVERLAY,                  false},
    {SCI_GETADDITIONALSELALPHA,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETADDITIONALCARETFORE,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETADDITIONALCARETSVISIBLE,                Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETCURRENTTEXT,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFONTQUALITY,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_SETFIRSTVISIBLELINE,                       Dispatch_effect::SCROLL,                   false},
    {SCI_GETMULTIPASTE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTAG,                                    Dispatch_effect::READ_ONLY,                false},
    {SCI_CONTRACTEDFOLDNEXT,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETIDENTIFIER,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERDEFINERGBAIMAGE,                     Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETTECHNOLOGY,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_COUNTCHARACTERS,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETCASEINSENSITIVEBEHAVIOUR,          Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETMULTI,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_AUTOCGETOPTIONS,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETRANGEPOINTER,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETGAPPOSITION,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_DELETERANGE,                               Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETWORDCHARS,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETWHITESPACECHARS,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPUNCTUATIONCHARS,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONEMPTY,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_VCHOMEDISPLAY,                             Dispatch_effect::OVERLAY,                  false},
    {SCI_VCHOMEDISPLAYEXTEND,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_GETCARETLINEVISIBLEALWAYS,                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEENDTYPESALLOWED,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEENDTYPESACTIVE,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_CHANGESELECTIONMODE,                       Dispatch_effect::OVERLAY,                  false},
    {SCI_AUTOCGETORDER,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETAUTOMATICFOLD,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETREPRESENTATION,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMOUSESELECTIONRECTANGULARSWITCH,        Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONRELATIVE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPHASESDRAW,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETNEXTTABSTOP,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETIMEINTERACTION,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETHOVERSTYLE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETHOVERFORE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETFLAGS,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_SETTARGETRANGE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTARGETTEXT,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_ISRANGEWORD,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETIDLESTYLING,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMOUSEWHEELCAPTURES,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTABDRAWMODE,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETACCESSIBILITY,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETLINEFRAME,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMOVEEXTENDSSELECTION,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_FOLDDISPLAYTEXTGETSTYLE,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETBIDIRECTIONAL,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETBIDIRECTIONAL,                          Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETLINECHARACTERINDEX,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_LINEFROMINDEXPOSITION,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_INDEXPOSITIONFROMLINE,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_COUNTCODEUNITS,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_POSITIONRELATIVECODEUNITS,                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCOMMANDEVENTS,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETMOVEEXTENDSSELECTION,                   Dispatch_effect::OVERLAY,                  false},
    {SCI_GETCHARACTERCATEGORYOPTIMIZATION,          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDEFAULTFOLDDISPLAYTEXT,                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTABMINIMUMWIDTH,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNSTARTVIRTUALSPACE,            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONNENDVIRTUALSPACE,              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTARGETSTARTVIRTUALSPACE,                Dispatch_effect::READ_ONLY,                false},
    {SCI_GETTARGETENDVIRTUALSPACE,                  Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERHANDLEFROMLINE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERNUMBERFROMLINE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_MARKERGETLAYER,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_EOLANNOTATIONGETTEXT,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_EOLANNOTATIONGETSTYLE,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_EOLANNOTATIONGETVISIBLE,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_EOLANNOTATIONGETSTYLEOFFSET,               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETMULTIEDGECOLUMN,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_SUPPORTSFEATURE,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_INDICGETSTROKEWIDTH,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETELEMENTCOLOUR,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETELEMENTISSET,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_GETELEMENTALLOWSTRANSLUCENT,               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETELEMENTBASECOLOUR,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETFONTLOCALE,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSELECTIONLAYER,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETLINELAYER,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETREPRESENTATIONAPPEARANCE,               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETREPRESENTATIONCOLOUR,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETDIRECTSTATUSFUNCTION,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCARETLINEHIGHLIGHTSUBLINE,              Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLAYOUTTHREADS,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSTYLEDTEXTFULL,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETCHANGEHISTORY,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUNDOSELECTIONHISTORY,                   Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOSELECTIONHISTORY,                   Dispatch_effect::READ_ONLY,                false},
    {SCI_SETSELECTIONSERIALIZED,                    Dispatch_effect::OVERLAY,                  false},
    {SCI_GETSELECTIONSERIALIZED,                    Dispatch_effect::READ_ONLY,                false},
    {SCI_GETUNDOACTIONS,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUNDOSAVEPOINT,                          Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOSAVEPOINT,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUNDODETACH,                             Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDODETACH,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUNDOTENTATIVE,                          Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOTENTATIVE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_SETUNDOCURRENT,                            Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOCURRENT,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_GETUNDOSEQUENCE,                           Dispatch_effect::READ_ONLY,                false},
    {SCI_PUSHUNDOACTIONTYPE,                        Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_CHANGELASTUNDOACTIONTEXT,                  Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETUNDOACTIONTYPE,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETUNDOACTIONPOSITION,                     Dispatch_effect::READ_ONLY,                false},
    {SCI_GETUNDOACTIONTEXT,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_CUTALLOWLINE,                              Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_SETCOPYSEPARATOR,                          Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_GETCOPYSEPARATOR,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_LINEINDENT,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_LINEDEDENT,                                Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_AUTOCSETIMAGESCALE,                        Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
    {SCI_AUTOCGETIMAGESCALE,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_SCROLLVERTICAL,                            Dispatch_effect::SCROLL,                   false},
    {SCI_GETDRAGDROPENABLED,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_SETDRAGDROPENABLED,                        Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
#ifndef SCI_DISABLE_PROVISIONAL
    {SCI_SETSCALETECHNIQUE,                         Dispatch_effect::STATIC_CONTENT_AND_STYLE, false},
#endif
#ifndef SCI_DISABLE_PROVISIONAL
    {SCI_GETSCALETECHNIQUE,                         Dispatch_effect::READ_ONLY,                false},
#endif
    {SCI_GETLEXER,                                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPROPERTY,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPROPERTYEXPANDED,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPROPERTYINT,                            Dispatch_effect::READ_ONLY,                false},
#ifdef SCI_GETSTYLEBITSNEEDED
    {SCI_GETSTYLEBITSNEEDED,                        Dispatch_effect::READ_ONLY,                false},
#endif
    {SCI_GETLEXERLANGUAGE,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_PROPERTYNAMES,                             Dispatch_effect::READ_ONLY,                false},
    {SCI_PROPERTYTYPE,                              Dispatch_effect::READ_ONLY,                false},
    {SCI_DESCRIBEPROPERTY,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_DESCRIBEKEYWORDSETS,                       Dispatch_effect::READ_ONLY,                false},
    {SCI_GETLINEENDTYPESSUPPORTED,                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSUBSTYLESSTART,                         Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSUBSTYLESLENGTH,                        Dispatch_effect::READ_ONLY,                false},
    {SCI_DISTANCETOSECONDARYSTYLES,                 Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSUBSTYLEBASES,                          Dispatch_effect::READ_ONLY,                false},
    {SCI_GETSTYLEFROMSUBSTYLE,                      Dispatch_effect::READ_ONLY,                false},
    {SCI_GETPRIMARYSTYLEFROMSTYLE,                  Dispatch_effect::READ_ONLY,                false},
    {SCI_GETNAMEDSTYLES,                            Dispatch_effect::READ_ONLY,                false},
    {SCI_NAMEOFSTYLE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_TAGSOFSTYLE,                               Dispatch_effect::READ_ONLY,                false},
    {SCI_DESCRIPTIONOFSTYLE,                        Dispatch_effect::READ_ONLY,                false},
};

constexpr bool message_rules_are_sorted()
{
    for (std::size_t i = 1; i < std::size(k_message_rules); ++i) {
        if (k_message_rules[i - 1].message >= k_message_rules[i].message) {
            return false;
        }
    }

    return true;
}

static_assert(message_rules_are_sorted(), "ScintillaQuick dispatch rules must be sorted and unique");

inline const Message_rule* find_message_rule(unsigned int message)
{
    const auto* end = std::end(k_message_rules);
    const auto* found = std::lower_bound(std::begin(k_message_rules), end, message,
        [](const Message_rule& rule, unsigned int value) { return rule.message < value; });
    return found != end && found->message == message ? found : nullptr;
}

constexpr scene_graph_update_request_info_t info_for_effect(Dispatch_effect effect)
{
    switch (effect) {
        case Dispatch_effect::READ_ONLY:                return {};
        case Dispatch_effect::OVERLAY:                  return {true, false, false, false};
        case Dispatch_effect::STATIC_CONTENT:           return {true, true,  false, false};
        case Dispatch_effect::STATIC_CONTENT_AND_STYLE: return {true, true,  true,  false};
        case Dispatch_effect::SCROLL:                   return {true, true,  true,  true};
    }

    return {true, true, true, false};
}

} // namespace detail

// Known read-only / query-like Scintilla messages that are safe to treat
// as non-visual. Messages listed with Dispatch_effect::READ_ONLY take
// the fast path and skip scene-graph resync.
inline bool scene_graph_message_is_known_read_only(unsigned int i_message)
{
    const detail::Message_rule* rule = detail::find_message_rule(i_message);
    return rule && rule->effect == detail::Dispatch_effect::READ_ONLY;
}

// Classification and scroll-width reset use the same rule entry.
inline scene_graph_update_request_info_t scene_graph_update_request(unsigned int i_message)
{
    if (const detail::Message_rule* rule = detail::find_message_rule(i_message)) {
        auto info = detail::info_for_effect(rule->effect);
        info.scroll_width_reset = rule->scroll_width_reset;
        return info;
    }

    // Unknown messages may mutate state, so they must request a full resync.
    return {true, true, true, false};
}

} // namespace Scintilla::Internal

#endif // SCINTILLAQUICK_DISPATCH_TABLE_H
