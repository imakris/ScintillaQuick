// Unit tests for the scene-graph update dispatch table
// (src/core/scintillaquick_dispatch_table.h).
//
// These tests deliberately do NOT spin up a Qt Quick window; the
// dispatch table is a pure function and we assert against it directly.
//
// Role of the dispatch table
// --------------------------
//
// The dispatch in scintillaquick_dispatch_table.h is a FAST-PATH
// PRE-SCHEDULER. For messages we statically know are mutators, the
// entries let `ScintillaQuickItem::send()` schedule a scene-graph
// update synchronously, without waiting for Scintilla's own
// `NotifyParent()` round-trip.
//
// The AUTHORITATIVE mutation-to-scene-graph link for every other
// message is Scintilla's own notification path
// (`ScintillaQuickCore::NotifyParent()` ->
// `ScintillaQuickItem::notifyParent()` ->
// `request_scene_graph_update(...)` for
// Modified/StyleNeeded/UpdateUI). That path fires for any Scintilla
// mutation regardless of how it was triggered, so messages NOT in the
// explicit allow-list fall through to `return {}` (no dispatch
// scheduling) and rely on the notification round-trip. That is the
// correct outcome for both:
//
//   * unknown mutators (correctness preserved by notification path)
//   * unknown read-only queries (no spurious repaint work, no
//     content_modified_since_last_capture churn, no cost to callers
//     who issue e.g. SCI_GETLENGTH / SCI_GETSELECTIONS /
//     SCI_GETMODEVENTMASK from outside the library).
//
// The tests below protect three invariants:
//
//   1. Known mutators in the allow-list return `needed=true`.
//   2. Unknown messages return `{}` (no dispatch scheduling), so the
//      public `send()` API stays cheap for read-only queries.
//   3. Messages issued from `syncQuickViewProperties()` and its
//      helper callees DO NOT return `needed=true`. If they did, the
//      nested `send()` call would re-enter `syncQuickViewProperties`
//      and recurse until the stack overflows. (The `send()` re-entry
//      guard catches this at runtime; this test catches it at build
//      time.)

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

void test_unknown_messages_take_no_op_fast_path()
{
    // THE public-API correctness invariant the reviewer flagged on
    // d7f1975: unknown messages must return `{}` so that
    // `editor.send(SCI_GETLENGTH)` / `send(SCI_GETSELECTIONS)` /
    // `send(SCI_GETMODEVENTMASK)` / etc. from application code do NOT
    // spuriously mark content dirty and schedule repaint work.
    //
    // For unknown MUTATORS this is also fine: Scintilla's own
    // NotifyParent() path catches the mutation and calls
    // request_scene_graph_update() from
    // ScintillaQuickItem::notifyParent(). See the header comment in
    // src/core/scintillaquick_dispatch_table.h for the full story.
    const unsigned int unknown_messages[] = {
        // Real SCI_GET* messages issued by external callers that the
        // library itself does not use internally. These are the exact
        // classes of message the reviewer called out.
        SCI_GETLENGTH,
        SCI_GETSELECTIONS,
        SCI_GETMODEVENTMASK,
        // A handful of other SCI_GET* messages that the library does
        // not currently whitelist anywhere.
        SCI_GETEOLMODE,
        SCI_GETMOUSEDOWNCAPTURES,
        SCI_GETTARGETSTART,
        SCI_GETTARGETEND,
        // Synthetic out-of-range numbers as a sanity check.
        0xFFFFu,
        0xFFFEu,
    };

    for (unsigned int msg : unknown_messages) {
        const scene_graph_update_request_t req = scene_graph_update_request(msg);
        if (req.needed) {
            std::fprintf(
                stderr,
                "FAIL: SCI_ message %u returned `needed=true` from the "
                "dispatch. Unknown messages MUST return `{}` so public "
                "`send()` API callers do not pay for spurious repaint "
                "scheduling on read-only queries. See the header "
                "comment in src/core/scintillaquick_dispatch_table.h.\n",
                msg);
        }
        SQ_EXPECT(!req.needed);
        SQ_EXPECT(!req.static_content_dirty);
        SQ_EXPECT(!req.needs_style_sync);
        SQ_EXPECT(!req.scrolling);
    }
}

// Regression: every SCI_* message that ScintillaQuickItem issues
// internally via `send()` from inside `syncQuickViewProperties()` or a
// getter called by it MUST NOT return `needed=true` from the dispatch.
// If it did, `send()` would call `syncQuickViewProperties()` again
// (through the re-entry guard) and the nested call would skip its work,
// which is strictly worse than the intended "notification path handles
// it" design.
//
// At the crash level, this used to be enforced by a big read-only
// allow-list; after the course correction, the invariant is enforced
// by the default-no-op branch, but we keep this test so that a future
// edit which accidentally adds e.g. SCI_TEXTHEIGHT to the mutator list
// surfaces immediately with a clear failure instead of crashing at
// runtime under the re-entry guard.
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
        const scene_graph_update_request_t req = scene_graph_update_request(msg);
        if (req.needed) {
            std::fprintf(
                stderr,
                "FAIL: SCI_ message %u returned `needed=true` from the "
                "dispatch, but it is issued from inside "
                "syncQuickViewProperties() via send(). That will "
                "re-enter syncQuickViewProperties() on the nested "
                "send() call. Either drop it from the mutator "
                "allow-list in scene_graph_update_request(), or stop "
                "calling send() for it from inside "
                "syncQuickViewProperties() and its helpers.\n",
                msg);
        }
        SQ_EXPECT(!req.needed);
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
    // SCI_GETLENGTH / SCI_GETSELECTIONS (the public-API callers the
    // reviewer flagged) must also not touch the scroll-width tracker.
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_GETLENGTH));
    SQ_EXPECT(!tracked_scroll_width_should_reset(SCI_GETSELECTIONS));
}

} // namespace

int main()
{
    test_known_mutating_messages_request_update();
    test_specific_scroll_dispatch();
    test_unknown_messages_take_no_op_fast_path();
    test_sync_quick_view_properties_path_is_recursion_safe();
    test_tracked_scroll_width_reset_table();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d dispatch table test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::puts("dispatch table tests: all checks passed.");
    return EXIT_SUCCESS;
}
