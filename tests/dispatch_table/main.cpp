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
    } while (0)

using Scintilla::Internal::scene_graph_update_request;
using Scintilla::Internal::scene_graph_update_request_t;
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
        const scene_graph_update_request_t req = scene_graph_update_request(msg);
        SQ_EXPECT(req.needed);
    }
}

void test_specific_scroll_dispatch()
{
    // SCI_SETFIRSTVISIBLELINE is the one message that sets `scrolling=true`.
    const scene_graph_update_request_t set_first = scene_graph_update_request(SCI_SETFIRSTVISIBLELINE);
    SQ_EXPECT(set_first.needed);
    SQ_EXPECT(set_first.static_content_dirty);
    SQ_EXPECT(set_first.needs_style_sync);
    SQ_EXPECT(set_first.scrolling);

    // SCI_SETXOFFSET dirties static content but is not treated as a
    // vertical scroll.
    const scene_graph_update_request_t set_xoff = scene_graph_update_request(SCI_SETXOFFSET);
    SQ_EXPECT(set_xoff.needed);
    SQ_EXPECT(set_xoff.static_content_dirty);
    SQ_EXPECT(!set_xoff.scrolling);
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

        const scene_graph_update_request_t req = scene_graph_update_request(msg);
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
    };

    for (unsigned int msg : public_queries) {
        SQ_EXPECT(scene_graph_message_is_known_read_only(msg));

        const scene_graph_update_request_t req = scene_graph_update_request(msg);
        SQ_EXPECT(!req.needed);
        SQ_EXPECT(!req.static_content_dirty);
        SQ_EXPECT(!req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

// Regression: every SCI_* message that ScintillaQuickItem issues
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
        const scene_graph_update_request_t req = scene_graph_update_request(msg);
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
        const scene_graph_update_request_t req = scene_graph_update_request(msg);
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

} // namespace

int main()
{
    test_known_mutating_messages_request_update();
    test_specific_scroll_dispatch();
    test_known_read_only_messages_take_fast_path();
    test_public_query_messages_take_fast_path();
    test_sync_quick_view_properties_path_is_recursion_safe();
    test_unknown_messages_trigger_conservative_full_resync();
    test_read_only_and_mutating_lists_are_disjoint();
    test_tracked_scroll_width_reset_table();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d dispatch table test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::puts("dispatch table tests: all checks passed.");
    return EXIT_SUCCESS;
}
