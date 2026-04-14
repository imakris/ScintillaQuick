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

#include "Scintilla.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

#define SQ_EXPECT(expr)                                                           \
    do {                                                                          \
        if (!(expr)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            ++g_failures;                                                         \
        }                                                                         \
    }                                                                             \
    while (0)

using Scintilla::Internal::scene_graph_update_request;
using Scintilla::Internal::scene_graph_update_request_info_t;
using Scintilla::Internal::scene_graph_message_is_known_read_only;
using Scintilla::Internal::tracked_scroll_width_should_reset;

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
            std::fprintf(
                stderr,
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
            std::fprintf(
                stderr,
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
// scheduling a resync" fall-through. That third case is the
// historical bug: a default branch that returned an empty request,
// meaning messages absent from the allow-list silently skipped the
// resync they needed.
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
    unsigned int classified_as_resync = 0;
    unsigned int silently_ignored = 0;

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
        std::fprintf(
            stderr,
            "FAIL: SCI_ message %u is neither known-read-only nor "
            "scheduling a resync. Add it to "
            "scene_graph_message_is_known_read_only() in "
            "src/core/scintillaquick_dispatch_table.h if it is a "
            "query, or make sure the conservative default still "
            "fires for it.\n",
            msg);
        SQ_EXPECT(false);
    }

    std::fprintf(
        stderr,
        "dispatch sweep: %u read-only, %u scheduled-resync, %u silently-ignored\n",
        classified_as_read_only,
        classified_as_resync,
        silently_ignored);

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
    test_sync_quick_view_properties_path_is_recursion_safe();
    test_unknown_messages_trigger_conservative_full_resync();
    test_read_only_and_mutating_lists_are_disjoint();
    test_tracked_scroll_width_reset_table();
    test_full_scintilla_message_range_is_classified();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d dispatch table test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::puts("dispatch table tests: all checks passed.");
    return EXIT_SUCCESS;
}
