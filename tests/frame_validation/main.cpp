// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_validation_access.h"

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
#include <vector>

#include "Scintilla.h"
#include "scintillaquick_font.h"
#include "scintillaquick_test_documents.h"

// This test exercises Scintilla::Internal render data directly, so keeping the
// internal namespace open here keeps the assertions readable.
using namespace Scintilla::Internal;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

int g_pass_count = 0;
int g_fail_count = 0;

struct Fixture_editor
{
    ScintillaQuick_item editor;

    Fixture_editor()
    {
        editor.setWidth(640);
        editor.setHeight(480);
        editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
        // Make caret visible for capture: give focus and disable blink.
        editor.send(SCI_SETFOCUS, 1);
        editor.send(SCI_SETCARETPERIOD, 0);
    }

    void set_text(const char* text)
    {
        editor.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(text));
        pump();
    }

    void pump()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    Render_frame capture()
    {
        pump();
        return ScintillaQuick_validation_access::capture_frame(editor);
    }

    Render_frame capture_cached()
    {
        return ScintillaQuick_validation_access::capture_cached_frame(editor);
    }
};

void send_key_press(
    ScintillaQuick_item &editor,
    int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(&editor, &event);
}

bool check(bool condition, const char* fixture_id, const char* detail)
{
    if (!condition) {
        qWarning("  FAIL [%s] %s", fixture_id, detail);
        ++g_fail_count;
        return false;
    }
    ++g_pass_count;
    return true;
}

bool wait_for_ready(QQuickWindow& window, Fixture_editor& fixture, const char* id, int timeout_ms = 2000)
{
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeout_ms) {
        window.raise();
        window.requestActivate();
        fixture.editor.forceActiveFocus();
        fixture.editor.send(SCI_SETFOCUS, 1);
        fixture.pump();

        if (window.isExposed() && fixture.editor.hasActiveFocus()) {
            return check(true, id, "fixture window must be exposed with editor focus");
        }
    }

    const QSize window_size = window.size();
    const QString detail = QStringLiteral(
        "fixture window readiness timed out: exposed=%1 editor_active_focus=%2 "
        "visible=%3 active=%4 window_size=%5x%6 editor_size=%7x%8")
        .arg(window.isExposed())
        .arg(fixture.editor.hasActiveFocus())
        .arg(window.isVisible())
        .arg(window.isActive())
        .arg(window_size.width())
        .arg(window_size.height())
        .arg(static_cast<int>(fixture.editor.width()))
        .arg(static_cast<int>(fixture.editor.height()));
    const QByteArray detail_utf8 = detail.toUtf8();
    return check(false, id, detail_utf8.constData());
}

QString reconstruct_line_text(const Visual_line_frame& line)
{
    QString result;
    for (const Text_run& run : line.text_runs) {
        result += run.text;
    }
    return result;
}

constexpr double k_geometry_tolerance = 1.5;

bool nearly_equal(double a, double b, double tolerance = k_geometry_tolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool rect_y_nearly_equal(const QRectF& a, const QRectF& b, double tolerance = k_geometry_tolerance)
{
    if (a.isNull() && b.isNull()) {
        return true;
    }
    return
        nearly_equal(a.top(), b.top(), tolerance) &&
        nearly_equal(a.bottom(), b.bottom(), tolerance);
}

struct Visual_line_signature
{
    Visual_line_key key;
    QString text;
    QRectF clip_rect;
};

std::vector<Visual_line_signature> visible_body_line_signatures(const Render_frame& frame)
{
    std::vector<Visual_line_signature> lines;
    for (const Visual_line_frame& line : frame.visual_lines) {
        if (!line.clip_rect.isValid()) {
            continue;
        }
        if (line.clip_rect.bottom() <= frame.text_rect.top() ||
            line.clip_rect.top()    >= frame.text_rect.bottom())
        {
            continue;
        }

        lines.push_back({line.key, reconstruct_line_text(line), line.clip_rect});
    }

    return lines;
}

bool check_visible_body_lines_match(
    const Render_frame& actual,
    const Render_frame& expected,
    const char* id)
{
    const std::vector<Visual_line_signature> actual_lines = visible_body_line_signatures(actual);
    const std::vector<Visual_line_signature> expected_lines = visible_body_line_signatures(expected);

    bool ok = true;
    ok &= check(!actual_lines.empty(), id, "actual frame must contain visible body text lines");
    ok &= check(!expected_lines.empty(), id, "expected frame must contain visible body text lines");
    ok &= check(actual_lines.size() == expected_lines.size(), id,
        "visible body line count must match expected frame");

    const size_t count = std::min(actual_lines.size(), expected_lines.size());
    for (size_t i = 0; i < count; ++i) {
        ok &= check(!actual_lines[i].text.trimmed().isEmpty(), id,
            "actual frame body text line must not be blank");
        ok &= check(actual_lines[i].key == expected_lines[i].key, id,
            "visual line key must match expected frame");
        ok &= check(actual_lines[i].text == expected_lines[i].text, id,
            "visual line text must match expected frame");
        ok &= check(rect_y_nearly_equal(actual_lines[i].clip_rect, expected_lines[i].clip_rect), id,
            "visual line clip_rect Y must match expected frame");
    }

    return ok;
}

bool rect_nearly_equal(const QRectF& a, const QRectF& b, double tolerance = k_geometry_tolerance)
{
    if (a.isNull() && b.isNull()) {
        return true;
    }
    return
        nearly_equal(a.left(),   b.left(),   tolerance) &&
        nearly_equal(a.right(),  b.right(),  tolerance) &&
        nearly_equal(a.top(),    b.top(),    tolerance) &&
        nearly_equal(a.bottom(), b.bottom(), tolerance);
}

const Visual_line_frame* find_visual_line(const Render_frame& frame, int document_line)
{
    for (const Visual_line_frame& line : frame.visual_lines) {
        if (line.key.document_line == document_line && line.key.subline_index == 0) {
            return &line;
        }
    }

    return nullptr;
}

const Text_run* first_non_empty_run(const Visual_line_frame* line)
{
    if (!line) {
        return nullptr;
    }

    for (const Text_run& run : line->text_runs) {
        if (!run.text.isEmpty()) {
            return &run;
        }
    }

    return nullptr;
}

void dump_frame_summary(const char* fixture_id, const Render_frame& frame)
{
    const auto n = [](auto v) { return static_cast<long long>(v); };
    qDebug(
        "  [%s] visual_lines=%lld  selections=%lld  carets=%lld  "
        "indicators=%lld  current_lines=%lld  markers=%lld  margin_texts=%lld  "
        "fold_texts=%lld  eol_annots=%lld  annots=%lld  ws_marks=%lld  "
        "decor_ul=%lld  indent_guides=%lld",
        fixture_id,
        n(frame.visual_lines.size()),
        n(frame.selection_primitives.size()),
        n(frame.caret_primitives.size()),
        n(frame.indicator_primitives.size()),
        n(frame.current_line_primitives.size()),
        n(frame.marker_primitives.size()),
        n(frame.margin_text_primitives.size()),
        n(frame.fold_display_texts.size()),
        n(frame.eol_annotations.size()),
        n(frame.annotations.size()),
        n(frame.whitespace_marks.size()),
        n(frame.decoration_underlines.size()),
        n(frame.indent_guides.size()));
}

// ---------------------------------------------------------------------------
// Semantic geometry helpers
// ---------------------------------------------------------------------------

// Check that no two text runs within any visual line overlap horizontally.
// Overlapping runs indicate stacked/garbled glyph output -- the exact
// regression that motivated this validation tranche.
bool check_no_overlapping_runs(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& vl : frame.visual_lines) {
        for (size_t i = 0; i < vl.text_runs.size(); ++i) {
            double a_left = vl.text_runs[i].position.x();
            double a_right = a_left + vl.text_runs[i].width;
            for (size_t j = i + 1; j < vl.text_runs.size(); ++j) {
                double b_left  = vl.text_runs[j].position.x();
                double b_right = b_left + vl.text_runs[j].width;
                double overlap = std::min(a_right, b_right) - std::max(a_left, b_left);
                ok &= check(overlap <= 0.1, id, "text runs overlap horizontally (stacked/garbled glyph risk)");
            }
        }
    }
    return ok;
}

// Check that every non-empty text run has positive width.
// Zero-width runs mean invisible text.
bool check_runs_positive_width(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& vl : frame.visual_lines) {
        for (const auto& run : vl.text_runs) {
            if (!run.text.isEmpty()) {
                ok &= check(run.width > 0.0, id, "non-empty text run has zero or negative width");
            }
        }
    }
    return ok;
}

// Check that text runs start within (or very near) their visual line's
// clip_rect horizontal bounds.
bool check_runs_within_line_clip(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& vl : frame.visual_lines) {
        if (!vl.clip_rect.isValid()) {
            continue;
        }
        for (const auto& run : vl.text_runs) {
            ok &= check(run.position.x() >= vl.clip_rect.left() - 2.0, id,
                "text run starts before visual line clip_rect left edge");
        }
    }
    return ok;
}

// For wrapped text: sublines of the same document line must have
// monotonically increasing Y positions matching their subline_index order.
bool check_sublines_ordered_by_y(const Render_frame& frame, const char* id)
{
    std::map<int, std::vector<const Visual_line_frame*>> by_doc_line;
    for (const auto& vl : frame.visual_lines) {
        by_doc_line[vl.key.document_line].push_back(&vl);
    }

    bool ok = true;
    for (const auto& [doc_line, lines] : by_doc_line) {
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            for (size_t j = i + 1; j < lines.size(); ++j) {
                if (lines[i]->key.subline_index < lines[j]->key.subline_index) {
                    ok &= check(lines[i]->origin.y() <= lines[j]->origin.y(), id,
                        "wrapped sublines not in Y order "
                        "(lower subline_index must have <= Y)");
                }
            }
        }
    }
    return ok;
}

// Each selection rectangle should vertically overlap at least one visual line.
bool check_selection_overlaps_visual_line(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& sel : frame.selection_primitives) {
        bool overlaps_any = false;
        for (const auto& vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) {
                continue;
            }
            if (sel.rect.bottom() > vl.clip_rect.top() && sel.rect.top() < vl.clip_rect.bottom()) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id, "selection rect does not vertically overlap any visual line");
    }
    return ok;
}

// Each caret rectangle should vertically overlap at least one visual line.
bool check_caret_within_line_bounds(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& caret : frame.caret_primitives) {
        bool overlaps_any = false;
        for (const auto& vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) {
                continue;
            }
            if (caret.rect.bottom() > vl.clip_rect.top() - 1.0 &&
                caret.rect.top() < vl.clip_rect.bottom() + 1.0)
            {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id, "caret rect does not vertically overlap any visual line");
    }
    return ok;
}

// Phase 2 contract: margin primitives must appear on the first wrapped
// subline only (subline_index == 0).
bool check_margin_first_subline_only(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& mt : frame.margin_text_primitives) {
        ok &= check(mt.subline_index == 0, id,
            "margin text primitive has subline_index != 0 "
            "(must be first subline only)");
    }
    return ok;
}

// Margin text top-Y should be close to the corresponding body visual line
// top-Y (within a small tolerance for baseline adjustments).
bool check_margin_y_aligns_with_body(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& mt : frame.margin_text_primitives) {
        bool found = false;
        for (const auto& vl : frame.visual_lines) {
            if (vl.key.document_line == mt.document_line && vl.key.subline_index == 0) {
                double y_diff = std::abs(mt.clip_rect.top() - vl.clip_rect.top());
                ok &= check(y_diff < 2.0, id,
                    "margin text Y not aligned with body visual line Y "
                    "(> 2px drift)");
                found = true;
                break;
            }
        }
        ok &= check(found, id, "margin text has no matching body visual line for its doc line");
    }
    return ok;
}

// Indicator rectangles should fall within the text area bounds.
bool check_indicator_within_text_area(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& ind : frame.indicator_primitives) {
        ok &= check(ind.rect.left()  >= frame.text_rect.left()  - 2.0, id, "indicator rect extends left of text area");
        ok &= check(ind.rect.right() <= frame.text_rect.right() + 2.0, id, "indicator rect extends right of text area");
    }
    return ok;
}

// Each indicator rectangle should vertically overlap at least one visual line.
bool check_indicator_overlaps_visual_line(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& ind : frame.indicator_primitives) {
        bool overlaps_any = false;
        for (const auto& vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) {
                continue;
            }
            if (ind.rect.bottom() > vl.clip_rect.top()  - 2.0 && ind.rect.top()  < vl.clip_rect.bottom() + 2.0 &&
                ind.rect.right()  > vl.clip_rect.left() - 2.0 && ind.rect.left() < vl.clip_rect.right()  + 2.0)
            {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id, "indicator rect does not overlap any visual line area");
    }
    return ok;
}

// For LTR text, text runs within a visual line should appear in
// monotonically increasing X order.
bool check_runs_x_ordered(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& vl : frame.visual_lines) {
        for (size_t i = 0; i + 1 < vl.text_runs.size(); ++i) {
            if (vl.text_runs[i].direction == Text_direction::left_to_right &&
                vl.text_runs[i + 1].direction == Text_direction::left_to_right)
            {
                ok &= check(vl.text_runs[i + 1].position.x() >= vl.text_runs[i].position.x() - 0.1, id,
                    "LTR text runs not in left-to-right X order");
            }
        }
    }
    return ok;
}

// Check that no two visual lines have vertically overlapping clip_rects.
// Overlapping clip_rects mean text from one line paints over another.
bool check_visual_lines_no_vertical_overlap(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (size_t i = 0; i < frame.visual_lines.size(); ++i) {
        const auto& a = frame.visual_lines[i];
        if (!a.clip_rect.isValid()) {
            continue;
        }
        for (size_t j = i + 1; j < frame.visual_lines.size(); ++j) {
            const auto& b = frame.visual_lines[j];
            if (!b.clip_rect.isValid()) {
                continue;
            }
            double v_overlap =
                std::min(a.clip_rect.bottom(), b.clip_rect.bottom()) -
                std::max(a.clip_rect.top(),    b.clip_rect.top());
            ok &= check(v_overlap <= 0.1, id,
                "visual lines have vertically overlapping clip rects "
                "(text from different lines would overlap)");
        }
    }
    return ok;
}

QColor scintilla_iprgb_color(int value)
{
    return QColor(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff);
}

int count_visual_lines_for_document_line(const Render_frame& frame, int document_line)
{
    return static_cast<int>(std::count_if(
        frame.visual_lines.begin(),
        frame.visual_lines.end(),
        [document_line](const Visual_line_frame& line) {
            return line.key.document_line == document_line;
        }));
}

const Visual_line_frame* find_matching_visual_line(
    const Render_frame& frame, const QRectF& rect, int document_line = -1)
{
    const Visual_line_frame* best = nullptr;
    double best_distance = std::numeric_limits<double>::max();

    for (const auto& line : frame.visual_lines) {
        if (!line.clip_rect.isValid()) {
            continue;
        }
        if (document_line >= 0 && line.key.document_line != document_line) {
            continue;
        }
        if (rect.bottom() < line.clip_rect.top()    - k_geometry_tolerance ||
            rect.top()    > line.clip_rect.bottom() + k_geometry_tolerance)
        {
            continue;
        }

        const double distance =
            std::abs(rect.top()    - line.clip_rect.top()) +
            std::abs(rect.bottom() - line.clip_rect.bottom());
        if (distance < best_distance) {
            best = &line;
            best_distance = distance;
        }
    }

    return best;
}

// Selection rects should fall within the text area's horizontal bounds.
bool check_selection_within_text_area(const Render_frame& frame, const char* id)
{
    bool ok = true;
    for (const auto& sel : frame.selection_primitives) {
        ok &= check(sel.rect.left()  >= frame.text_rect.left()  - 2.0, id,
            "selection rect extends left of text area");
        ok &= check(sel.rect.right() <= frame.text_rect.right() + 2.0, id,
            "selection rect extends right of text area");
    }
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static bool test_plain_ascii_short()
{
    const char* id = "plain_ascii_short";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("alpha beta gamma");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check( frame.visual_lines[0].key.document_line == 0, id, "first line document_line must be 0");
        ok &= check( frame.visual_lines[0].key.subline_index == 0, id, "first line subline_index must be 0");
        ok &= check(!frame.visual_lines[0].text_runs.empty(),      id, "first line must have text runs");
        QString text = reconstruct_line_text(frame.visual_lines[0]);
        ok &= check(text.contains("alpha"), id, "text must contain 'alpha'");
        ok &= check(text.contains("gamma"), id, "text must contain 'gamma'");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_runs_within_line_clip(frame, id);
    ok &= check_runs_x_ordered(frame, id);
    return ok;
}

static bool test_plain_ascii_long_wrap()
{
    const char* id = "plain_ascii_long_wrap";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    // Long paragraph that should wrap into at least 3 sublines at 200px.
    const char *long_text =
        "The quick brown fox jumps over the lazy dog and then runs around "
        "the garden several more times until the text is long enough to "
        "definitely wrap into multiple visual sublines at a narrow width.";
    f.set_text(long_text);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.visual_lines.size() >= 3, id, "must produce >= 3 visual lines from wrap");

    int max_subline = 0;
    for (const Visual_line_frame& vl : frame.visual_lines) {
        if (vl.key.document_line == 0 && vl.key.subline_index > max_subline) {
            max_subline = vl.key.subline_index;
        }
    }
    ok &= check(max_subline >= 2, id, "must have subline_index >= 2 for doc line 0");

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_cached_frame_wrap_toggle_rebuilds()
{
    const char* id = "cached_frame_wrap_toggle_rebuilds";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.set_text(
        "The quick brown fox jumps over the lazy dog and then keeps running "
        "until this single document line must wrap into several visual sublines.");

    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    const Render_frame wrapped_frame = f.capture_cached();

    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    const Render_frame unwrapped_frame = f.capture_cached();
    dump_frame_summary(id, unwrapped_frame);

    int wrapped_doc_line_count = 0;
    int unwrapped_doc_line_count = 0;
    int max_unwrapped_subline = 0;
    for (const Visual_line_frame& line : wrapped_frame.visual_lines) {
        if (line.key.document_line == 0) {
            ++wrapped_doc_line_count;
        }
    }
    for (const Visual_line_frame& line : unwrapped_frame.visual_lines) {
        if (line.key.document_line == 0) {
            ++unwrapped_doc_line_count;
            max_unwrapped_subline = std::max(max_unwrapped_subline, line.key.subline_index);
        }
    }

    bool ok = true;
    ok &= check(wrapped_doc_line_count >= 2, id,
        "wrap mode must produce multiple cached visual sublines");
    ok &= check(unwrapped_doc_line_count == 1, id,
        "turning wrap off must rebuild cached visual lines");
    ok &= check(max_unwrapped_subline == 0, id,
        "unwrapped document line must not keep stale wrapped subline indexes");
    ok &= check_no_overlapping_runs(unwrapped_frame, id);
    ok &= check_visual_lines_no_vertical_overlap(unwrapped_frame, id);
    return ok;
}

static bool test_horizontal_scroll_resets_on_doc_switch()
{
    const char* id = "horizontal_scroll_resets_on_doc_switch";
    qDebug("--- %s", id);

    Fixture_editor f;

    QByteArray long_line("prefix ");
    long_line += QByteArray(400, 'x');
    f.set_text(long_line.constData());

    Render_frame long_frame = f.capture();
    dump_frame_summary(id, long_frame);

    bool ok = true;
    const int long_viewport_width = static_cast<int>(std::ceil(long_frame.text_rect.width()));
    const int long_scroll_width = static_cast<int>(f.editor.send(SCI_GETSCROLLWIDTH));
    ok &= check(long_scroll_width > long_viewport_width, id,
        "long document must widen horizontal scroll width beyond the viewport");

    const sptr_t short_doc = f.editor.send(SCI_CREATEDOCUMENT, 0, 0);
    ok &= check(short_doc != 0, id, "must create a second document");
    if (!short_doc) {
        return ok;
    }

    f.editor.send(SCI_ADDREFDOCUMENT, 0, short_doc);
    f.editor.send(SCI_SETDOCPOINTER, 0, short_doc);
    f.editor.send(SCI_RELEASEDOCUMENT, 0, short_doc);
    f.set_text("short line\nanother short line");

    Render_frame short_frame = f.capture();
    const int short_viewport_width = static_cast<int>(std::ceil(short_frame.text_rect.width()));
    const int short_scroll_width   = static_cast<int>(f.editor.send(SCI_GETSCROLLWIDTH));
    const int short_x_offset       = static_cast<int>(f.editor.send(SCI_GETXOFFSET));

    ok &= check(short_scroll_width <= short_viewport_width + 1, id,
        "short replacement document must not keep phantom horizontal scroll range");
    ok &= check(short_x_offset == 0, id,
        "document switch to a short file must clamp horizontal offset back to zero");
    ok &= check_no_overlapping_runs(short_frame, id);
    ok &= check_visual_lines_no_vertical_overlap(short_frame, id);
    return ok;
}

static bool test_caret_left_scrolls_to_long_previous_line()
{
    const char* id = "caret_left_scrolls_to_long_previous_line";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(220);

    QByteArray long_line("long ");
    long_line += QByteArray(320, 'x');
    QByteArray text("top\n");
    text += long_line;
    text += "\nend";
    f.set_text(text.constData());

    const Sci::Position previous_line_end = static_cast<Sci::Position>(f.editor.send(SCI_GETLINEENDPOSITION, 1));
    const Sci::Position next_line_start = static_cast<Sci::Position>(f.editor.send(SCI_POSITIONFROMLINE, 2));
    f.editor.send(SCI_GOTOPOS, next_line_start);
    const int before_x_offset = static_cast<int>(f.editor.send(SCI_GETXOFFSET));

    send_key_press(f.editor, Qt::Key_Left);

    const Sci::Position current_pos = static_cast<Sci::Position>(f.editor.send(SCI_GETCURRENTPOS));
    const int current_x_offset = static_cast<int>(f.editor.send(SCI_GETXOFFSET));
    const Render_frame cached_after = f.capture_cached();
    const Render_frame direct_after = f.capture();
    dump_frame_summary(id, cached_after);

    const Text_run* cached_long_run = first_non_empty_run(find_visual_line(cached_after, 1));
    const Text_run* direct_long_run = first_non_empty_run(find_visual_line(direct_after, 1));

    bool ok = true;
    ok &= check(current_pos == previous_line_end, id,
        "left arrow from the next line start must move to the end of the previous line");
    ok &= check(current_x_offset > before_x_offset, id,
        "moving to a long previous line must increase horizontal offset");
    ok &= check(cached_long_run != nullptr, id,
        "cached frame must contain the long previous line after the left-arrow move");
    ok &= check(direct_long_run != nullptr, id,
        "direct frame must contain the long previous line after the left-arrow move");
    if (cached_long_run && direct_long_run) {
        ok &= check(std::abs(cached_long_run->position.x() - direct_long_run->position.x()) <= 0.5, id,
            "cached frame text layout must match direct capture after horizontal caret scroll");
    }
    ok &= check_no_overlapping_runs(cached_after, id);
    ok &= check_visual_lines_no_vertical_overlap(cached_after, id);
    return ok;
}

static bool test_cached_overlay_only_selection_refreshes_overlay()
{
    const char* id = "cached_overlay_only_selection_refreshes_overlay";
    qDebug("--- %s", id);

    QQuickWindow window;
    Fixture_editor f;
    bool ok = true;
    window.setColor(Qt::white);
    window.resize(640, 160);
    f.editor.setParentItem(window.contentItem());
    f.editor.setHeight(160);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 48);
    f.editor.send(SCI_SETCARETWIDTH, 2);
    window.show();
    ok &= wait_for_ready(window, f, id);
    f.set_text(
        "alpha bravo charlie\n"
        "second stable line\n"
        "third stable line\n"
        "fourth stable line");
    f.editor.send(SCI_SETSEL, 0, 0);

    const Render_frame fresh_before = f.capture();
    const Render_frame cached_before = f.capture_cached();

    f.editor.send(SCI_SETSEL, 6, 11);
    const Render_frame cached_after = f.capture_cached();
    const Render_frame fresh_after = f.capture();
    dump_frame_summary(id, cached_after);

    ok &= check_visible_body_lines_match(cached_before, fresh_before, id);
    ok &= check_visible_body_lines_match(cached_after, cached_before, id);
    ok &= check_visible_body_lines_match(cached_after, fresh_after, id);

    ok &= check(cached_before.selection_primitives.empty(), id,
        "fixture must start without a selection primitive");
    ok &= check(!cached_after.selection_primitives.empty(), id,
        "cached overlay-only frame must include the new selection primitive");
    ok &= check(cached_after.selection_primitives.size() == fresh_after.selection_primitives.size(), id,
        "cached overlay-only selection count must match fresh capture");
    ok &= check(cached_after.caret_primitives.size() == fresh_after.caret_primitives.size(), id,
        "cached overlay-only caret count must match fresh capture");
    ok &= check(!cached_after.caret_primitives.empty(), id,
        "cached overlay-only frame must include the moved caret primitive");

    const size_t selection_count =
        std::min(cached_after.selection_primitives.size(), fresh_after.selection_primitives.size());
    for (size_t i = 0; i < selection_count; ++i) {
        const Selection_primitive& cached_selection = cached_after.selection_primitives[i];
        const Selection_primitive& fresh_selection  = fresh_after.selection_primitives[i];
        ok &= check(rect_nearly_equal(cached_selection.rect, fresh_selection.rect), id,
            "cached overlay-only selection rect must match fresh capture");
        ok &= check(cached_selection.color == fresh_selection.color, id,
            "cached overlay-only selection color must match fresh capture");
        ok &= check(cached_selection.is_main == fresh_selection.is_main, id,
            "cached overlay-only selection main flag must match fresh capture");
    }

    const size_t caret_count =
        std::min(cached_after.caret_primitives.size(), fresh_after.caret_primitives.size());
    for (size_t i = 0; i < caret_count; ++i) {
        const Caret_primitive& cached_caret = cached_after.caret_primitives[i];
        const Caret_primitive& fresh_caret  = fresh_after.caret_primitives[i];
        ok &= check(rect_nearly_equal(cached_caret.rect, fresh_caret.rect), id,
            "cached overlay-only caret rect must match fresh capture");
        ok &= check(cached_caret.color == fresh_caret.color, id,
            "cached overlay-only caret color must match fresh capture");
        ok &= check(cached_caret.is_main == fresh_caret.is_main, id,
            "cached overlay-only caret main flag must match fresh capture");
    }
    if (!cached_before.caret_primitives.empty() && !cached_after.caret_primitives.empty()) {
        const bool caret_moved = !rect_nearly_equal(
            cached_before.caret_primitives.front().rect,
            cached_after.caret_primitives.front().rect);
        ok &= check(caret_moved, id,
            "cached overlay-only caret rect must move after SCI_SETSEL");
    }

    ok &= check_selection_overlaps_visual_line(cached_after, id);
    ok &= check_selection_within_text_area(cached_after, id);
    ok &= check_caret_within_line_bounds(cached_after, id);
    f.editor.setParentItem(nullptr);
    window.close();
    f.pump();
    return ok;
}

// This validates scrolled full-capture parity against direct capture, including
// normal body text and secondary primitives.
static bool test_scrolled_full_capture_matches_direct_secondary_geometry()
{
    const char* id = "scrolled_full_capture_matches_direct_secondary_geometry";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setHeight(160);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    f.editor.send(SCI_SETVIEWWS, SCWS_VISIBLEALWAYS);
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 48);
    f.editor.send(SCI_SETINDENTATIONGUIDES, SC_IV_REAL);
    f.editor.send(SCI_SETTABWIDTH, 4);
    f.editor.send(SCI_SETINDENT, 4);
    f.editor.send(SCI_STYLESETUNDERLINE, 1, 1);
    f.editor.send(SCI_STYLESETFORE, 1, 0x0000FF);

    QByteArray text;
    int indicator_start = -1;
    int underline_start = -1;
    for (int line = 0; line < 32; ++line) {
        text += QByteArray((line % 3) * 4, ' ');
        QByteArray prefix("line ");
        prefix += QByteArray::number(line);
        prefix += ' ';
        text += prefix;
        if (line == 3) {
            indicator_start = text.size();
            text += "indicator";
        } else if (line == 4) {
            underline_start = text.size();
            text += "underlined";
        }
        else {
            text += "plain";
        }
        text += ' ';
        text += QByteArray((line % 4) + 1, ' ');
        text += "body";
        if ((line % 2) == 0) {
            text += "\twith-tab";
        }
        text += " \x01 represented\n";
    }

    bool ok = true;
    f.set_text(text.constData());
    ok &= check(indicator_start >= 0, id, "fixture must record indicator start position");
    ok &= check(underline_start >= 0, id, "fixture must record underline start position");
    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_PLAIN);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, indicator_start, 9);
    f.editor.send(SCI_STARTSTYLING, underline_start);
    f.editor.send(SCI_SETSTYLING, 10, 1);
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD);
    const char* annotation_text = "scroll update annotation";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 4, reinterpret_cast<sptr_t>(annotation_text));
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
    const char* eol_annotation_text = "scroll update eol";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 5, reinterpret_cast<sptr_t>(eol_annotation_text));
    f.pump();

    const int initial_first_visible_line = static_cast<int>(f.editor.send(SCI_GETFIRSTVISIBLELINE));
    const Render_frame initial_item_frame = f.capture_cached();
    f.editor.scrollVertical(initial_first_visible_line + 1);
    const int scrolled_first_visible_line = static_cast<int>(f.editor.send(SCI_GETFIRSTVISIBLELINE));
    const Render_frame item_after = f.capture_cached();
    const Render_frame direct_after = f.capture();
    dump_frame_summary(id, item_after);

    ok &= check(!initial_item_frame.visual_lines.empty(), id, "initial item frame must contain visual lines");
    ok &= check(scrolled_first_visible_line == initial_first_visible_line + 1, id,
        "fixture must scroll by exactly one visible line");
    ok &= check_visible_body_lines_match(item_after, direct_after, id);
    ok &= check(item_after.indicator_primitives.size() == direct_after.indicator_primitives.size(), id,
        "indicator primitive count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.whitespace_marks.size() == direct_after.whitespace_marks.size(), id,
        "whitespace mark count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.margin_text_primitives.size() == direct_after.margin_text_primitives.size(), id,
        "margin text primitive count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.annotations.size() == direct_after.annotations.size(), id,
        "annotation primitive count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.eol_annotations.size() == direct_after.eol_annotations.size(), id,
        "EOL annotation primitive count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.indent_guides.size() == direct_after.indent_guides.size(), id,
        "indent guide primitive count must match direct capture after scrolled item-frame update");
    ok &= check(item_after.decoration_underlines.size() == direct_after.decoration_underlines.size(), id,
        "decoration underline primitive count must match direct capture after scrolled item-frame update");

    bool checked_represented_run = false;
    for (const Visual_line_frame& item_line : item_after.visual_lines) {
        const Visual_line_frame* direct_line = find_visual_line(direct_after, item_line.key.document_line);
        if (!direct_line) {
            continue;
        }

        const size_t run_count = std::min(item_line.text_runs.size(), direct_line->text_runs.size());
        for (size_t i = 0; i < run_count; ++i) {
            const Text_run& item_run = item_line.text_runs[i];
            const Text_run& direct_run = direct_line->text_runs[i];
            if (!item_run.is_represented_text && !item_run.represented_as_blob) {
                continue;
            }

            ok &= check(nearly_equal(item_run.position.y(), direct_run.position.y()), id,
                "represented text run position.y must match direct capture after scrolled item-frame update");
            ok &= check(nearly_equal(item_run.top, direct_run.top), id,
                "represented text run top must match direct capture after scrolled item-frame update");
            ok &= check(nearly_equal(item_run.bottom, direct_run.bottom), id,
                "represented text run bottom must match direct capture after scrolled item-frame update");
            ok &= check(rect_y_nearly_equal(item_run.blob_text_clip_rect, direct_run.blob_text_clip_rect), id,
                "represented text clip rect must match direct capture after scrolled item-frame update");
            ok &= check(rect_y_nearly_equal(item_run.blob_outer_rect, direct_run.blob_outer_rect), id,
                "represented blob outer rect must match direct capture after scrolled item-frame update");
            ok &= check(rect_y_nearly_equal(item_run.blob_inner_rect, direct_run.blob_inner_rect), id,
                "represented blob inner rect must match direct capture after scrolled item-frame update");
            checked_represented_run = true;
        }
    }
    ok &= check(checked_represented_run, id,
        "fixture must include at least one represented text/blob run in the scrolled item frame");

    bool checked_indicator = false;
    for (const Indicator_primitive& item_indicator : item_after.indicator_primitives) {
        for (const Indicator_primitive& direct_indicator : direct_after.indicator_primitives) {
            if (!rect_y_nearly_equal(item_indicator.rect, direct_indicator.rect)) {
                continue;
            }

            ok &= check(rect_y_nearly_equal(item_indicator.line_rect, direct_indicator.line_rect), id,
                "indicator line_rect must match direct capture after scrolled item-frame update");
            ok &= check(rect_y_nearly_equal(item_indicator.character_rect, direct_indicator.character_rect), id,
                "indicator character_rect must match direct capture after scrolled item-frame update");
            checked_indicator = true;
            break;
        }
    }
    ok &= check(checked_indicator, id,
        "fixture must include at least one indicator primitive in the scrolled item frame");

    bool checked_whitespace = false;
    for (const Whitespace_mark_primitive& item_ws : item_after.whitespace_marks) {
        const Visual_line_frame* item_line = find_matching_visual_line(item_after, item_ws.rect);
        if (!item_line) {
            continue;
        }

        const Whitespace_mark_primitive* direct_match = nullptr;
        for (const Whitespace_mark_primitive& direct_ws : direct_after.whitespace_marks) {
            if (direct_ws.kind != item_ws.kind ||
                !nearly_equal(direct_ws.rect.left(), item_ws.rect.left()) ||
                !nearly_equal(direct_ws.rect.right(), item_ws.rect.right()))
            {
                continue;
            }

            const Visual_line_frame* direct_line = find_matching_visual_line(direct_after, direct_ws.rect);
            if (direct_line &&
                direct_line->key.document_line == item_line->key.document_line &&
                direct_line->key.subline_index == item_line->key.subline_index)
            {
                direct_match = &direct_ws;
                break;
            }
        }

        if (!direct_match) {
            continue;
        }

        ok &= check(rect_y_nearly_equal(item_ws.rect, direct_match->rect), id,
            "whitespace mark rect must match direct capture after scrolled item-frame update");
        ok &= check(nearly_equal(item_ws.mid_y, direct_match->mid_y), id,
            "whitespace mark mid_y must match direct capture after scrolled item-frame update");
        checked_whitespace = true;
    }
    ok &= check(checked_whitespace, id,
        "fixture must include at least one matched whitespace mark in the scrolled item frame");

    bool checked_margin_text = false;
    for (const Margin_text_primitive& item_margin : item_after.margin_text_primitives) {
        const auto direct_it = std::find_if(
            direct_after.margin_text_primitives.begin(),
            direct_after.margin_text_primitives.end(),
            [&item_margin](const Margin_text_primitive& direct_margin) {
                return direct_margin.document_line == item_margin.document_line &&
                    direct_margin.subline_index == item_margin.subline_index &&
                    direct_margin.text == item_margin.text;
            });
        if (direct_it == direct_after.margin_text_primitives.end()) {
            continue;
        }

        ok &= check(nearly_equal(item_margin.position.y(), direct_it->position.y()), id,
            "margin text position.y must match direct capture after scrolled item-frame update");
        ok &= check(rect_y_nearly_equal(item_margin.clip_rect, direct_it->clip_rect), id,
            "margin text clip_rect must match direct capture after scrolled item-frame update");
        ok &= check(nearly_equal(item_margin.baseline_y, direct_it->baseline_y), id,
            "margin text baseline_y must match direct capture after scrolled item-frame update");
        checked_margin_text = true;
    }
    ok &= check(checked_margin_text, id,
        "fixture must include at least one matched margin text primitive in the scrolled item frame");

    bool checked_annotation = false;
    for (const Annotation_primitive& item_annotation : item_after.annotations) {
        const auto direct_it = std::find_if(
            direct_after.annotations.begin(),
            direct_after.annotations.end(),
            [&item_annotation](const Annotation_primitive& direct_annotation) {
                return direct_annotation.document_line == item_annotation.document_line &&
                    direct_annotation.annotation_line == item_annotation.annotation_line &&
                    direct_annotation.text == item_annotation.text;
            });
        if (direct_it == direct_after.annotations.end()) {
            continue;
        }

        ok &= check(nearly_equal(item_annotation.position.y(), direct_it->position.y()), id,
            "annotation position.y must match direct capture after scrolled item-frame update");
        ok &= check(rect_y_nearly_equal(item_annotation.rect, direct_it->rect), id,
            "annotation rect must match direct capture after scrolled item-frame update");
        ok &= check(nearly_equal(item_annotation.baseline_y, direct_it->baseline_y), id,
            "annotation baseline_y must match direct capture after scrolled item-frame update");
        checked_annotation = true;
    }
    ok &= check(checked_annotation, id,
        "fixture must include at least one matched annotation primitive in the scrolled item frame");

    bool checked_eol_annotation = false;
    for (const Eol_annotation_primitive& item_eol : item_after.eol_annotations) {
        const auto direct_it = std::find_if(
            direct_after.eol_annotations.begin(),
            direct_after.eol_annotations.end(),
            [&item_eol](const Eol_annotation_primitive& direct_eol) {
                return direct_eol.document_line == item_eol.document_line &&
                    direct_eol.text == item_eol.text;
            });
        if (direct_it == direct_after.eol_annotations.end()) {
            continue;
        }

        ok &= check(nearly_equal(item_eol.position.y(), direct_it->position.y()), id,
            "EOL annotation position.y must match direct capture after scrolled item-frame update");
        ok &= check(rect_y_nearly_equal(item_eol.rect, direct_it->rect), id,
            "EOL annotation rect must match direct capture after scrolled item-frame update");
        ok &= check(nearly_equal(item_eol.baseline_y, direct_it->baseline_y), id,
            "EOL annotation baseline_y must match direct capture after scrolled item-frame update");
        checked_eol_annotation = true;
    }
    ok &= check(checked_eol_annotation, id,
        "fixture must include at least one matched EOL annotation primitive in the scrolled item frame");

    ok &= check(!item_after.indent_guides.empty(), id,
        "fixture must include indent guide primitives in the scrolled item frame");
    bool checked_indent_guide = false;
    for (const Indent_guide_primitive& item_guide : item_after.indent_guides) {
        const QRectF item_rect(
            item_guide.x,
            item_guide.top,
            1.0,
            item_guide.bottom - item_guide.top);
        const Visual_line_frame* item_line = find_matching_visual_line(item_after, item_rect);
        if (!item_line) {
            continue;
        }

        const Indent_guide_primitive* direct_match = nullptr;
        for (const Indent_guide_primitive& direct_guide : direct_after.indent_guides) {
            if (!nearly_equal(direct_guide.x, item_guide.x)) {
                continue;
            }

            const QRectF direct_rect(
                direct_guide.x,
                direct_guide.top,
                1.0,
                direct_guide.bottom - direct_guide.top);
            const Visual_line_frame* direct_line = find_matching_visual_line(direct_after, direct_rect);
            if (direct_line &&
                direct_line->key.document_line == item_line->key.document_line &&
                direct_line->key.subline_index == item_line->key.subline_index)
            {
                direct_match = &direct_guide;
                break;
            }
        }

        if (!direct_match) {
            continue;
        }

        ok &= check(nearly_equal(item_guide.top, direct_match->top), id,
            "indent guide top must match direct capture after scrolled item-frame update");
        ok &= check(nearly_equal(item_guide.bottom, direct_match->bottom), id,
            "indent guide bottom must match direct capture after scrolled item-frame update");
        checked_indent_guide = true;
    }
    ok &= check(checked_indent_guide, id,
        "fixture must include at least one matched indent guide in the scrolled item frame");

    bool checked_underline = false;
    for (const Decoration_underline_primitive& item_underline : item_after.decoration_underlines) {
        const auto direct_it = std::find_if(
            direct_after.decoration_underlines.begin(),
            direct_after.decoration_underlines.end(),
            [&item_underline](const Decoration_underline_primitive& direct_underline) {
                return direct_underline.kind == item_underline.kind &&
                    nearly_equal(direct_underline.rect.left(), item_underline.rect.left()) &&
                    nearly_equal(direct_underline.rect.right(), item_underline.rect.right());
            });
        if (direct_it == direct_after.decoration_underlines.end()) {
            continue;
        }

        ok &= check(rect_y_nearly_equal(item_underline.rect, direct_it->rect), id,
            "decoration underline rect must match direct capture after scrolled item-frame update");
        checked_underline = true;
    }
    ok &= check(checked_underline, id,
        "fixture must include at least one matched decoration underline in the scrolled item frame");

    ok &= check_no_overlapping_runs(item_after, id);
    ok &= check_visual_lines_no_vertical_overlap(item_after, id);
    return ok;
}

static bool test_edit_savepoint_one_line_scroll_matches_fresh_body_text()
{
    const char* id = "edit_savepoint_one_line_scroll_matches_fresh_body_text";
    qDebug("--- %s", id);

    const int initial_first_visible_line = 120;
    const int edited_line                = 126;
    const QByteArray inserted_line("inserted visible line\n");

    const auto configure_editor = [](Fixture_editor& fixture) {
        fixture.editor.setHeight(160);
        fixture.editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
        fixture.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
        fixture.editor.send(SCI_SETMARGINWIDTHN, 0, 48);
        fixture.editor.send(SCI_SETCARETWIDTH, 0);
    };

    const QString original_text = build_large_document(500);
    QString final_text = original_text;
    const QString edit_label = QStringLiteral("Line %1:").arg(edited_line + 1, 6, 10, QLatin1Char('0'));
    const int insertion_pos = final_text.indexOf(edit_label);
    bool ok = true;
    ok &= check(insertion_pos >= 0, id, "fixture must find the edited visible body line");
    if (insertion_pos < 0) {
        return false;
    }
    final_text.insert(insertion_pos, QString::fromLatin1(inserted_line));

    Fixture_editor f;
    configure_editor(f);
    const QByteArray original_utf8 = original_text.toUtf8();
    f.set_text(original_utf8.constData());
    f.editor.send(SCI_SETFIRSTVISIBLELINE, initial_first_visible_line);
    const Render_frame initial_item_frame = f.capture_cached();

    const int first_visible_before_edit = static_cast<int>(f.editor.send(SCI_GETFIRSTVISIBLELINE));
    ok &= check(first_visible_before_edit == initial_first_visible_line, id,
        "fixture must start at first visible line 120 before the edit");
    ok &= check(!visible_body_line_signatures(initial_item_frame).empty(), id,
        "initial item frame must contain visible body text lines");

    const sptr_t edit_pos = f.editor.send(SCI_POSITIONFROMLINE, edited_line);
    ok &= check(edit_pos >= 0, id, "fixture must resolve the edited visible body line position");
    if (edit_pos < 0) {
        return false;
    }

    f.editor.send(SCI_INSERTTEXT, edit_pos, reinterpret_cast<sptr_t>(inserted_line.constData()));
    // Historical repro included a savepoint; these assertions do not depend on savepoint state.
    f.editor.send(SCI_SETSAVEPOINT);
    const Render_frame edited_item_frame = f.capture_cached();
    ok &= check(!visible_body_line_signatures(edited_item_frame).empty(), id,
        "edited item frame must contain visible body text lines before scrolling");

    const int first_visible_before_scroll = static_cast<int>(f.editor.send(SCI_GETFIRSTVISIBLELINE));
    ok &= check(first_visible_before_scroll == initial_first_visible_line, id,
        "edit/savepoint must leave first visible line unchanged before the one-line scroll");

    f.editor.scrollVertical(first_visible_before_scroll + 1);
    const int first_visible_after_scroll = static_cast<int>(f.editor.send(SCI_GETFIRSTVISIBLELINE));
    const Render_frame scrolled_item_frame = f.capture_cached();
    dump_frame_summary(id, scrolled_item_frame);

    ok &= check(first_visible_after_scroll == first_visible_before_scroll + 1, id,
        "fixture must scroll by exactly one visible line after edit/savepoint");

    Fixture_editor reference;
    configure_editor(reference);
    const QByteArray final_utf8 = final_text.toUtf8();
    reference.set_text(final_utf8.constData());
    reference.editor.send(SCI_SETFIRSTVISIBLELINE, first_visible_after_scroll);
    const Render_frame fresh_reference_frame = reference.capture();

    ok &= check_visible_body_lines_match(scrolled_item_frame, fresh_reference_frame, id);
    ok &= check_no_overlapping_runs(scrolled_item_frame, id);
    ok &= check_visual_lines_no_vertical_overlap(scrolled_item_frame, id);
    return ok;
}

static bool test_mixed_styles_wrap()
{
    const char* id = "mixed_styles_wrap";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    // Configure distinct styles.
    const QByteArray font_family = scintillaquick::shared::deterministic_test_font_family_utf8();
    f.editor.send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(font_family.constData()));
    f.editor.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    f.editor.send(SCI_STYLECLEARALL);
    f.editor.send(SCI_STYLESETFORE, 1, 0x000000);
    f.editor.send(SCI_STYLESETBOLD, 1, 1);
    f.editor.send(SCI_STYLESETFORE, 2, 0x0000FF);
    f.editor.send(SCI_STYLESETITALIC, 2, 1);
    f.editor.send(SCI_STYLESETFORE, 3, 0xFF0000);

    const char* text =
        "keyword identifier literal keyword identifier literal "
        "keyword identifier literal keyword identifier literal end";
    f.set_text(text);

    // Apply styles: cycle through 3 styles across words.
    int styles[] = {1, 2, 3};
    int pos = 0;
    const int text_len = static_cast<int>(strlen(text));
    int word_index = 0;
    while (pos < text_len) {
        if (text[pos] == ' ') {
            ++pos;
            continue;
        }
        int word_len = 0;
        while (pos + word_len < text_len && text[pos + word_len] != ' ') {
            ++word_len;
        }
        int style = styles[word_index % 3];
        f.editor.send(SCI_STARTSTYLING, pos);
        f.editor.send(SCI_SETSTYLING, word_len, style);
        pos += word_len;
        ++word_index;
    }

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.visual_lines.size() >= 2, id, "styled text must wrap into >= 2 visual lines");

    // Check that at least one visual line has runs with different style_ids.
    bool found_multi_style_line = false;
    for (const Visual_line_frame& vl : frame.visual_lines) {
        if (vl.text_runs.size() >= 2) {
            int first_style = vl.text_runs[0].style_id;
            for (size_t i = 1; i < vl.text_runs.size(); ++i) {
                if (vl.text_runs[i].style_id != first_style) {
                    found_multi_style_line = true;
                    break;
                }
            }
        }
        if (found_multi_style_line) {
            break;
        }
    }
    ok &= check(found_multi_style_line, id, "at least one visual line must have runs with different style_ids");

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_tab_layout_default()
{
    const char* id = "tab_layout_default";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETTABWIDTH, 8);
    f.set_text("col1\tcol2\tcol3");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check(!frame.visual_lines[0].text_runs.empty(), id, "first line must have text runs");
        QString text = reconstruct_line_text(frame.visual_lines[0]);
        ok &= check(text.contains("col1"), id, "text must contain 'col1'");
        ok &= check(text.contains("col3"), id, "text must contain 'col3'");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    return ok;
}

static bool test_tab_layout_nondefault()
{
    const char* id = "tab_layout_nondefault";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETTABWIDTH, 4);
    f.set_text("col1\tcol2\tcol3");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check(!frame.visual_lines[0].text_runs.empty(), id, "first line must have text runs");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    return ok;
}

static bool test_selection_single_line()
{
    const char* id = "selection_single_line";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("select this word here");
    // Select "this" (positions 7..11).
    f.editor.send(SCI_SETSEL, 7, 11);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.selection_primitives.size() == 1, id,
        "single-line selection must produce exactly one selection primitive");
    if (!frame.selection_primitives.empty()) {
        const Selection_primitive& sel = frame.selection_primitives[0];
        const int selection_start    = 7;
        const int selection_end      = 11;
        const int selection_line     = static_cast<int   >(f.editor.send(SCI_LINEFROMPOSITION,      selection_start));
        const double expected_left   = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, selection_start));
        const double expected_right  = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, selection_end));
        const double expected_top    = static_cast<double>(
            f.editor.send(SCI_POINTYFROMPOSITION, 0, f.editor.send(SCI_POSITIONFROMLINE, selection_line)));
        const double expected_bottom =
            expected_top + static_cast<double>(f.editor.send(SCI_TEXTHEIGHT, selection_line));
        const Visual_line_frame* line = find_matching_visual_line(frame, sel.rect, selection_line);

        ok &= check(sel.is_main, id, "single-line selection must be marked as main");
        ok &= check(sel.rect.width()  > 0, id,
            "selection rect width must be > 0");
        ok &= check(sel.rect.height() > 0, id,
            "selection rect height must be > 0");
        ok &= check(line != nullptr, id,
            "selection rect must map to a visual line");
        ok &= check(line && line->key.document_line == selection_line, id,
            "selection rect must map to the selected document line");
        ok &= check(nearly_equal(sel.rect.left(), expected_left), id,
            "selection rect left edge must match the selected text start");
        ok &= check(nearly_equal(sel.rect.right(), expected_right), id,
            "selection rect right edge must match the selected text end");
        ok &= check(nearly_equal(sel.rect.top(), expected_top), id,
            "selection rect top must match the selected line top");
        ok &= check(nearly_equal(sel.rect.bottom(), expected_bottom), id,
            "selection rect bottom must match the selected line bottom");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_selection_overlaps_visual_line(frame, id);
    ok &= check_selection_within_text_area(frame, id);
    return ok;
}

static bool test_selection_wrap_boundary()
{
    const char* id = "selection_wrap_boundary";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    const char *text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field to ensure we get enough wrapping here.";
    f.set_text(text);

    // Select a range spanning the wrap boundary (middle third of text).
    int text_len = static_cast<int>(strlen(text));
    f.editor.send(SCI_SETSEL, text_len / 3, 2 * text_len / 3);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.selection_primitives.size() >= 2, id,
        "wrap-spanning selection must produce multiple selection primitives");
    if (!frame.selection_primitives.empty()) {
        std::map<long long, int> visual_line_counts;
        for (const auto& sel : frame.selection_primitives) {
            const Visual_line_frame* line = find_matching_visual_line(frame, sel.rect);
            ok &= check(sel.rect.width()  > 0, id, "wrap-spanning selection rect width must be > 0");
            ok &= check(sel.rect.height() > 0, id, "wrap-spanning selection rect height must be > 0");
            ok &= check(line != nullptr,       id, "wrap-spanning selection rect must map to a visual line");
            if (line) {
                const long long key =
                    (static_cast<long long>(line->key.document_line) << 32) |
                    static_cast<unsigned int>(line->key.subline_index);
                ++visual_line_counts[key];
            }
        }
        ok &= check(visual_line_counts.size() >= 2, id,
            "wrap-spanning selection must cover multiple visual sublines");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_selection_overlaps_visual_line(frame, id);
    ok &= check_selection_within_text_area(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_caret_mid_line()
{
    const char* id = "caret_mid_line";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("caret goes here in the middle of styled text");
    // Place caret at position 16 (no selection).
    f.editor.send(SCI_GOTOPOS, 16);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.caret_primitives.empty(), id, "caret_primitives must not be empty");
    if (!frame.caret_primitives.empty()) {
        const Caret_primitive& caret = frame.caret_primitives[0];
        ok &= check(caret.rect.height() > 0, id, "caret rect height must be > 0");
        // Caret should be within text bounds.
        ok &= check(caret.rect.left() >= frame.text_rect.left(), id, "caret left must be >= text_rect left");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_caret_within_line_bounds(frame, id);
    return ok;
}

static bool test_caret_wrap_continuation()
{
    const char* id = "caret_wrap_continuation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    const char* text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field to test caret on a wrapped continuation.";
    f.set_text(text);

    // Move caret to the latter part (should be on a continuation subline).
    int text_len = static_cast<int>(strlen(text));
    f.editor.send(SCI_GOTOPOS, text_len - 10);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.caret_primitives.empty(), id, "caret_primitives must not be empty on continuation");
    if (!frame.caret_primitives.empty()) {
        const Caret_primitive& caret = frame.caret_primitives[0];
        ok &= check(caret.rect.height() > 0, id, "caret rect height must be > 0");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_caret_within_line_bounds(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_margin_numbers_basic()
{
    const char* id = "margin_numbers_basic";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 40);
    f.set_text("line one\nline two\nline three\nline four\nline five");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(
        !frame.margin_text_primitives.empty(), id, "margin_text_primitives must not be empty with line numbers on");
    ok &= check(frame.margin_text_primitives.size() == 5, id,
        "numbered margin must produce exactly one primitive per document line");

    // Check that margin primitives contain expected line-number text.
    bool found_1 = false, found_2 = false;
    for (const Margin_text_primitive& m : frame.margin_text_primitives) {
        QString trimmed = m.text.trimmed();
        if (trimmed == "1") {
            found_1 = true;
        }
        if (trimmed == "2") {
            found_2 = true;
        }
        ok &= check(m.clip_rect.width()  > 0, id, "margin clip_rect width must be > 0");
        ok &= check(m.clip_rect.height() > 0, id, "margin clip_rect height must be > 0");
    }
    ok &= check(found_1, id, "margin must contain line number '1'");
    ok &= check(found_2, id, "margin must contain line number '2'");

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_margin_first_subline_only(frame, id);
    ok &= check_margin_y_aligns_with_body(frame, id);
    return ok;
}

static bool test_margin_numbers_wrap()
{
    const char* id = "margin_numbers_wrap";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 40);

    f.set_text(
        "This is a very long first line that should definitely wrap into "
        "multiple visual sublines when shown at narrow width.\n"
        "Short second line.\n"
        "Third line here.");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.margin_text_primitives.size() == 3, id,
        "wrapped line numbers must produce exactly one primitive per document line");

    // Phase 2 contract: one margin primitive per numbered document line,
    // first wrapped subline only.  Verify that we do not get duplicate
    // line-numbers for the same document line.
    std::map<int, int> doc_line_count;
    for (const Margin_text_primitive& m : frame.margin_text_primitives) {
        doc_line_count[m.document_line]++;
    }
    for (const auto& [doc_line, count] : doc_line_count) {
        if (count > 1) {
            qWarning("  [%s] document_line %d has %d margin primitives (expected 1 per doc line)",
                id, doc_line, count);
        }
        ok &= check(count == 1, id, "each document line should have exactly one margin primitive");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_margin_first_subline_only(frame, id);
    ok &= check_margin_y_aligns_with_body(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_plain_indicator_basic()
{
    const char* id = "plain_indicator_basic";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("indicator on this text here");

    // Set up a captured plain indicator on "this text" (pos 13..22).
    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_PLAIN);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 13, 9);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.indicator_primitives.size() == 1, id,
        "plain indicator test must produce exactly one indicator primitive");
    if (!frame.indicator_primitives.empty()) {
        const Indicator_primitive& ind = frame.indicator_primitives[0];
        const int indicator_start = 13;
        const int indicator_end   = 22;
        const int indicator_line  = static_cast<int   >(f.editor.send(SCI_LINEFROMPOSITION,      indicator_start));
        const double expected_left   = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, indicator_start));
        const double expected_right  = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, indicator_end));
        const double expected_top    = static_cast<double>(
            f.editor.send(SCI_POINTYFROMPOSITION, 0, f.editor.send(SCI_POSITIONFROMLINE, indicator_line)));
        const double expected_bottom =
            expected_top + static_cast<double>(f.editor.send(SCI_TEXTHEIGHT, indicator_line));
        const Visual_line_frame* line = find_matching_visual_line(frame, ind.rect, indicator_line);

        ok &= check(ind.rect.width()  > 0, id,
            "indicator rect width must be > 0");
        ok &= check(ind.rect.height() > 0, id,
            "indicator rect height must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0x0000FF), id,
            "indicator color must match the configured indicator color");
        ok &= check(ind.indicator_number == 0, id,
            "indicator number must be 0");
        ok &= check(ind.indicator_style  == static_cast<int>(INDIC_PLAIN), id,
            "indicator style must be plain");
        ok &= check(line != nullptr, id,
            "indicator rect must map to a visual line");
        ok &= check(line && line->key.document_line == indicator_line, id,
            "indicator rect must map to the expected document line");
        ok &= check(nearly_equal(ind.rect.left(),  expected_left),  id,
            "indicator rect left edge must match the indicator start");
        ok &= check(nearly_equal(ind.rect.right(), expected_right), id,
            "indicator rect right edge must match the indicator end");
        ok &= check(ind.rect.top()    >= expected_top    - k_geometry_tolerance, id,
            "indicator rect top must stay within the indicator line");
        ok &= check(ind.rect.bottom() <= expected_bottom + k_geometry_tolerance, id,
            "indicator rect bottom must stay within the indicator line");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_indicator_within_text_area(frame, id);
    ok &= check_indicator_overlaps_visual_line(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 3 fixtures: multi-selection, rectangular selection, current-line
// ---------------------------------------------------------------------------

static bool test_multi_selection()
{
    const char* id = "multi_selection";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("aaa bbb ccc ddd eee fff");
    // Add three separate selections (multi-select).
    f.editor.send(SCI_SETSELECTION, 0, 3);  // "aaa"
    f.editor.send(SCI_ADDSELECTION, 4, 7);  // "bbb"
    f.editor.send(SCI_ADDSELECTION, 8, 11); // "ccc"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.selection_primitives.size() == 3, id,
        "must have exactly 3 selection primitives for 3 selections");

    // Check that exactly one selection is marked as main and that all three
    // selections are on the same line but do not overlap horizontally.
    int main_count = 0;
    std::vector<QRectF> rects;
    for (const auto& sel : frame.selection_primitives) {
        ok &= check(sel.rect.width()  > 0, id, "selection rect width must be > 0");
        ok &= check(sel.rect.height() > 0, id, "selection rect height must be > 0");
        ok &= check(
                sel.rect.top() ==            frame.selection_primitives.front().rect.top() ||
                nearly_equal(sel.rect.top(), frame.selection_primitives.front().rect.top()),
            id, "multi-selection rects must sit on the same visual line");
        if (sel.is_main) {
            ++main_count;
        }
        rects.push_back(sel.rect);
    }
    ok &= check(main_count == 1, id, "exactly one selection must be marked as main");
    for (size_t i = 0; i < rects.size(); ++i) {
        for (size_t j = i + 1; j < rects.size(); ++j) {
            const bool disjoint =
                rects[i].right() <= rects[j].left() + k_geometry_tolerance ||
                rects[j].right() <= rects[i].left() + k_geometry_tolerance;
            ok &= check(disjoint, id, "multi-selection rects must not overlap horizontally");
        }
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_selection_overlaps_visual_line(frame, id);
    ok &= check_selection_within_text_area(frame, id);
    return ok;
}

static bool test_rectangular_selection()
{
    const char* id = "rectangular_selection";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    // Set a rectangular selection across lines.
    f.editor.send(SCI_SETRECTANGULARSELECTIONANCHOR, 0);
    f.editor.send(SCI_SETRECTANGULARSELECTIONCARET, 22);
    f.editor.send(SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, 0);
    f.editor.send(SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, 0);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(
        frame.selection_primitives.size() == 3, id, "rectangular selection must produce exactly three primitives");
    if (!frame.selection_primitives.empty()) {
        const double reference_left = frame.selection_primitives.front().rect.left();
        const double reference_right = frame.selection_primitives.front().rect.right();
        std::map<int, int> document_line_counts;
        int main_count = 0;

        for (const auto& sel : frame.selection_primitives) {
            const Visual_line_frame* line = find_matching_visual_line(frame, sel.rect);
            ok &= check(sel.rect.width()  > 0, id,
                "rectangular selection rect width must be > 0");
            ok &= check(sel.rect.height() > 0, id,
                "rectangular selection rect height must be > 0");
            ok &= check(nearly_equal(sel.rect.left(), reference_left), id,
                "rectangular selection rects must share a common left edge");
            ok &= check(nearly_equal(sel.rect.right(), reference_right), id,
                "rectangular selection rects must share a common right edge");
            ok &= check(line != nullptr, id, "rectangular selection rect must map to a visual line");
            if (line) {
                ++document_line_counts[line->key.document_line];
            }
            if (sel.is_main) {
                ++main_count;
            }
        }

        ok &= check(main_count == 1, id,
            "rectangular selection must have exactly one main primitive");
        ok &= check(document_line_counts.size() == 3, id,
            "rectangular selection must span exactly three document lines");
        ok &= check(document_line_counts[0] == 1, id,
            "rectangular selection must cover document line 0 exactly once");
        ok &= check(document_line_counts[1] == 1, id,
            "rectangular selection must cover document line 1 exactly once");
        ok &= check(document_line_counts[2] == 1, id,
            "rectangular selection must cover document line 2 exactly once");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_selection_overlaps_visual_line(frame, id);
    ok &= check_selection_within_text_area(frame, id);
    return ok;
}

static bool test_current_line_frame()
{
    const char* id = "current_line_frame";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETCARETLINEVISIBLE, 1);
    f.editor.send(SCI_SETCARETLINEBACK, 0xFFFFE0);
    f.editor.send(SCI_SETCARETLINEFRAME, 1);
    f.set_text("first line\nsecond line\nthird line");
    f.editor.send(SCI_GOTOPOS, 5);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    const int caret_position             = static_cast<int>(f.editor.send(SCI_GETCURRENTPOS));
    const int caret_line                 = static_cast<int>(f.editor.send(SCI_LINEFROMPOSITION, caret_position));
    const int expected_visual_line_count = count_visual_lines_for_document_line(frame, caret_line);

    ok &= check(frame.current_line_primitives.size() == static_cast<size_t>(expected_visual_line_count), id,
        "current-line highlight must produce one primitive per visual subline");
    ok &= check(expected_visual_line_count >= 1, id, "caret line must exist in the captured frame");
    if (!frame.current_line_primitives.empty()) {
        for (const auto& cl : frame.current_line_primitives) {
            const Visual_line_frame* line = find_matching_visual_line(frame, cl.rect, caret_line);
            ok &= check(cl.rect.width()  > 0, id,
                "current line rect width must be > 0");
            ok &= check(cl.rect.height() > 0, id,
                "current line rect height must be > 0");
            ok &= check(cl.color == scintilla_iprgb_color(0xFFFFE0), id,
                "current line color must match the configured caret-line color");
            ok &= check(cl.framed, id,
                "current line primitive must preserve framed caret-line styling");
            ok &= check(nearly_equal(cl.rect.left(), frame.text_rect.left()), id,
                "current line rect must start at the text area left edge");
            ok &= check(nearly_equal(cl.rect.right(), frame.text_rect.right()), id,
                "current line rect must end at the text area right edge");
            ok &= check(line != nullptr, id,
                "current line rect must map to a visual line");
            ok &= check(line && line->key.document_line == caret_line, id,
                "current line rect must map to the caret document line");
        }
    }

    // Current line should overlap a visual line.
    for (const auto& cl : frame.current_line_primitives) {
        bool overlaps_any = false;
        for (const auto& vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) {
                continue;
            }
            if (cl.rect.bottom() > vl.clip_rect.top() && cl.rect.top() < vl.clip_rect.bottom()) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id, "current line rect must overlap at least one visual line");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 4 fixtures: expanded indicator styles
// ---------------------------------------------------------------------------

static bool test_squiggle_indicator()
{
    const char* id = "squiggle_indicator";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("error on this word here");

    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 9, 4); // "this"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.indicator_primitives.size() == 1, id,
        "squiggle indicator test must produce exactly one indicator primitive");
    if (!frame.indicator_primitives.empty()) {
        const Indicator_primitive& ind = frame.indicator_primitives[0];
        const Visual_line_frame* line = find_matching_visual_line(frame, ind.rect, 0);
        ok &= check(ind.rect.width()  > 0, id,
            "squiggle indicator rect width must be > 0");
        ok &= check(ind.rect.height() > 0, id,
            "squiggle indicator rect height must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0x0000FF), id,
            "squiggle indicator color must match the configured indicator color");
        ok &= check(ind.indicator_style == static_cast<int>(INDIC_SQUIGGLE), id,
            "indicator_style must be INDIC_SQUIGGLE");
        ok &= check(ind.indicator_number == 0, id,
            "indicator_number must be 0");
        ok &= check(ind.under_text, id,
            "squiggle indicator must stay in the under-text layer");
        ok &= check(line != nullptr, id,
            "squiggle indicator rect must map to a visual line");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_indicator_within_text_area(frame, id);
    ok &= check_indicator_overlaps_visual_line(frame, id);
    return ok;
}

static bool test_box_indicator()
{
    const char* id = "box_indicator";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("box around this word");

    f.editor.send(SCI_INDICSETSTYLE,       1, INDIC_BOX);
    f.editor.send(SCI_INDICSETFORE,        1, 0xFF0000);
    f.editor.send(SCI_INDICSETUNDER,       1, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 1);
    f.editor.send(SCI_INDICATORFILLRANGE,  4, 6); // "around"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(
        frame.indicator_primitives.size() == 1, id, "box indicator test must produce exactly one indicator primitive");
    if (!frame.indicator_primitives.empty()) {
        const Indicator_primitive& ind = frame.indicator_primitives[0];
        const Visual_line_frame* line = find_matching_visual_line(frame, ind.rect, 0);
        ok &= check(ind.rect.width() > 0, id,
            "box indicator rect width must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0xFF0000), id,
            "box indicator color must match the configured indicator color");
        ok &= check(ind.indicator_style == static_cast<int>(INDIC_BOX), id,
            "indicator_style must be INDIC_BOX");
        ok &= check(ind.indicator_number == 1, id,
            "indicator_number must be 1");
        ok &= check(ind.under_text, id,
            "box indicator must stay in the under-text layer");
        ok &= check(line != nullptr, id,
            "box indicator rect must map to a visual line");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_indicator_within_text_area(frame, id);
    ok &= check_indicator_overlaps_visual_line(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 5 fixture: marker symbol
// ---------------------------------------------------------------------------

static bool test_marker_symbol()
{
    const char* id = "marker_symbol";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    f.editor.send(SCI_SETMARGINWIDTHN, 1, 16);
    f.editor.send(SCI_SETMARGINMASKN, 1, 0x01); // marker 0 only

    f.editor.send(SCI_MARKERDEFINE, 0, SC_MARK_CIRCLE);
    f.editor.send(SCI_MARKERSETFORE, 0, 0x000000);
    f.editor.send(SCI_MARKERSETBACK, 0, 0xFF0000);

    f.set_text("marked line\nunmarked line\nthird line");
    f.editor.send(SCI_MARKERADD, 0, 0); // add marker 0 to line 0

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.marker_primitives.size() == 1, id, "marker fixture must produce exactly one marker primitive");
    if (!frame.marker_primitives.empty()) {
        const Marker_primitive& m = frame.marker_primitives[0];
        const Visual_line_frame* line = find_matching_visual_line(frame, m.rect, 0);
        const double expected_top     = line ? line->clip_rect.top()    : 0.0;
        const double expected_bottom  = line ? line->clip_rect.bottom() : 0.0;
        ok &= check(m.rect.width()  > 0, id, "marker rect width must be > 0");
        ok &= check(m.rect.height() > 0, id, "marker rect height must be > 0");
        ok &= check(m.marker_number == 0, id, "marker_number must be 0");
        ok &= check(m.marker_type == static_cast<int>(SC_MARK_CIRCLE), id, "marker_type must be SC_MARK_CIRCLE");
        ok &= check(m.document_line == 0, id, "marker must be on document line 0");
        ok &= check(m.foreground == scintilla_iprgb_color(0x000000), id,
            "marker foreground color must match the configured marker color");
        ok &= check(m.background == scintilla_iprgb_color(0xFF0000), id,
            "marker background color must match the configured marker color");
        ok &= check(line != nullptr, id, "marker rect must map to a visual line");
        ok &= check(nearly_equal(m.rect.top(),    expected_top),    id,
            "marker rect top must match the target line");
        ok &= check(nearly_equal(m.rect.bottom(), expected_bottom), id,
            "marker rect bottom must match the target line");
        ok &= check(m.rect.right() <= frame.text_rect.left() + 2.0, id, "marker rect must stay within the gutter");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 6 fixture: multi-caret
// ---------------------------------------------------------------------------

static bool test_multi_caret()
{
    const char* id = "multi_caret";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("abc def ghi jkl");
    // Place main caret at position 4, add additional carets.
    f.editor.send(SCI_SETSELECTION, 4, 4);
    f.editor.send(SCI_ADDSELECTION, 8, 8);
    f.editor.send(SCI_ADDSELECTION, 12, 12);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(
        frame.caret_primitives.size() == 3, id, "multi-caret fixture must produce exactly three caret primitives");

    // Check that exactly one caret is marked as main.
    int main_count = 0;
    int caret_line = -1;
    std::vector<QRectF> caret_rects;
    for (const auto& caret : frame.caret_primitives) {
        if (caret.is_main) {
            ++main_count;
        }
        ok &= check(caret.rect.width()  > 0, id, "each caret rect width must be > 0");
        ok &= check(caret.rect.height() > 0, id, "each caret rect height must be > 0");
        const Visual_line_frame* line = find_matching_visual_line(frame, caret.rect);
        ok &= check(line != nullptr, id, "each caret rect must map to a visual line");
        if (line) {
            if (caret_line < 0) {
                caret_line = line->key.document_line;
            }
            ok &= check(line->key.document_line == caret_line, id,
                "all carets must stay on the same document line in this fixture");
            ok &= check(nearly_equal(caret.rect.top(), frame.caret_primitives.front().rect.top()), id,
                "multi-caret rects must stay on the same visual line");
        }
        caret_rects.push_back(caret.rect);
    }
    ok &= check(main_count == 1, id, "exactly one caret must be marked as main");
    for (size_t i = 0; i < caret_rects.size(); ++i) {
        for (size_t j = i + 1; j < caret_rects.size(); ++j) {
            const bool disjoint = caret_rects[i].right() <= caret_rects[j].left() + k_geometry_tolerance ||
                                  caret_rects[j].right() <= caret_rects[i].left() + k_geometry_tolerance;
            ok &= check(disjoint, id, "multi-caret rects must not overlap horizontally");
        }
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_caret_within_line_bounds(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: whitespace visibility
// ---------------------------------------------------------------------------

static bool test_whitespace_visible()
{
    const char* id = "whitespace_visible";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETVIEWWS, SCWS_VISIBLEALWAYS);
    f.set_text("a b\tc");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.whitespace_marks.empty(), id, "visible whitespace must produce whitespace_marks");

    bool has_dot = false;
    bool has_tab = false;
    for (const auto& ws : frame.whitespace_marks) {
        ok &= check(ws.rect.width()  > 0, id, "whitespace mark rect width must be > 0");
        ok &= check(ws.rect.height() > 0, id, "whitespace mark rect height must be > 0");
        ok &= check(ws.color.isValid() && ws.color.alpha() > 0, id, "whitespace mark color must be valid and visible");
        if (ws.kind == Whitespace_mark_kind_t::space_dot) {
            has_dot = true;
        }
        if (ws.kind == Whitespace_mark_kind_t::tab_arrow) {
            has_tab = true;
        }
    }
    ok &= check(has_dot, id, "whitespace_marks must include a space_dot");
    ok &= check(has_tab, id, "whitespace_marks must include a tab_arrow");

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: EOL annotation
// ---------------------------------------------------------------------------

static bool test_eol_annotation()
{
    const char* id = "eol_annotation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\n");
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
    const char* annot_text = "this is an eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.eol_annotations.empty(), id, "EOL annotation must produce eol_annotations");
    if (!frame.eol_annotations.empty()) {
        const Eol_annotation_primitive& eol = frame.eol_annotations[0];
        ok &= check(eol.text == QString::fromUtf8(annot_text), id, "EOL annotation text must match");
        ok &= check(eol.rect.width() > 0, id, "EOL annotation rect width must be > 0");
        ok &= check(eol.document_line == 0, id, "EOL annotation must be on document line 0");
        ok &= check(eol.foreground.isValid(), id, "EOL annotation foreground must be valid");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: annotation (multi-line below line)
// ---------------------------------------------------------------------------

static bool test_annotation()
{
    const char* id = "annotation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD);
    const char* annot_text = "annotation text here";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id, "annotation must produce annotations");
    if (!frame.annotations.empty()) {
        const Annotation_primitive& annot = frame.annotations[0];
        ok &= check(annot.text == QString::fromUtf8(annot_text), id, "annotation text must match");
        ok &= check(annot.rect.width() > 0, id, "annotation rect width must be > 0");
        ok &= check(annot.document_line == 0, id, "annotation must be on document line 0");
        ok &= check(annot.foreground.isValid(), id, "annotation foreground must be valid");
    }

    ok &= check_no_overlapping_runs(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: indent guide
// ---------------------------------------------------------------------------

static bool test_indent_guide()
{
    const char* id = "indent_guide";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETINDENTATIONGUIDES, SC_IV_REAL);
    f.editor.send(SCI_SETTABWIDTH, 4);
    f.editor.send(SCI_SETINDENT, 4);
    // Two levels of indentation so guides appear at the 4-space boundary.
    f.set_text("if (x) {\n    if (y) {\n        z = 1;\n    }\n}");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.indent_guides.empty(), id, "indent guides must produce indent_guides when SC_IV_REAL is set");
    for (const auto& guide : frame.indent_guides) {
        ok &= check(guide.top < guide.bottom, id, "indent guide top must be < bottom");
        ok &= check(guide.x > 0, id, "indent guide x must be positive");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: decoration underlines (style underline)
// ---------------------------------------------------------------------------

static bool test_style_underline()
{
    const char* id = "style_underline";
    qDebug("--- %s", id);

    Fixture_editor f;
    // Enable underline on style 1
    f.editor.send(SCI_STYLESETUNDERLINE, 1, 1);
    f.editor.send(SCI_STYLESETFORE, 1, 0x0000FF);
    f.set_text("normal text");
    // Apply style 1 to "normal"
    f.editor.send(SCI_STARTSTYLING, 0);
    f.editor.send(SCI_SETSTYLING, 6, 1);
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.decoration_underlines.empty(), id, "style underline must produce decoration_underlines");
    if (!frame.decoration_underlines.empty()) {
        const Decoration_underline_primitive& ul = frame.decoration_underlines[0];
        ok &= check(ul.rect.width() > 0, id, "underline rect width must be > 0");
        ok &= check(ul.kind == Decoration_kind_t::style_underline, id, "underline kind must be style_underline");
        ok &= check(ul.color.isValid(), id, "underline color must be valid");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 8: fold display text with box outline
// ---------------------------------------------------------------------------

static bool test_fold_display_text()
{
    const char* id = "fold_display_text";
    qDebug("--- %s", id);

    Fixture_editor f;
    // Set fold display text style
    f.editor.send(SCI_SETDEFAULTFOLDDISPLAYTEXT, 0, reinterpret_cast<sptr_t>("..."));
    f.editor.send(SCI_FOLDDISPLAYTEXTSETSTYLE, SC_FOLDDISPLAYTEXT_BOXED, 0);

    // Create foldable structure via explicit fold levels (no lexer needed)
    f.set_text("header line\n    body1\n    body2\nclosing");
    f.editor.send(SCI_SETFOLDLEVEL, 0, SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
    f.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);
    f.pump();

    // Collapse fold at line 0
    f.editor.send(SCI_FOLDLINE, 0, SC_FOLDACTION_CONTRACT);
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    // When folded, fold display text should appear
    ok &= check(!frame.fold_display_texts.empty(), id, "collapsing a fold must produce fold_display_texts");
    if (!frame.fold_display_texts.empty()) {
        const Fold_display_text_primitive& fold = frame.fold_display_texts[0];
        ok &= check(!fold.text.isEmpty(), id, "fold display text must have non-empty text");
        ok &= check(fold.rect.width()  > 0, id, "fold display text rect width must be > 0");
        ok &= check(fold.rect.height() > 0, id, "fold display text rect height must be > 0");
        ok &= check(fold.foreground.isValid(), id, "fold display text must have valid foreground color");
        ok &= check(fold.document_line == 0, id, "fold display text must be on the folded line");
        ok &= check(fold.boxed, id, "fold display text must have boxed=true for SC_FOLDDISPLAYTEXT_BOXED");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 8: marker fold_part semantics
// ---------------------------------------------------------------------------

static bool test_marker_fold_part()
{
    const char* id = "marker_fold_part";
    qDebug("--- %s", id);

    Fixture_editor f;
    // Set up fold margin with box-connected markers
    f.editor.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    f.editor.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    f.editor.send(SCI_SETMARGINWIDTHN, 2, 16);

    // Configure fold markers as box style
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    f.editor.send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

    // Create foldable structure
    f.set_text("function a() {\n    line1\n    line2\n}");
    f.editor.send(SCI_SETFOLDLEVEL, 0, SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
    f.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);

    // Enable fold block highlighting and place caret inside the fold body
    // so HighlightDelimiter populates fold parts.
    f.editor.send(SCI_MARKERENABLEHIGHLIGHT, 1);
    f.editor.send(SCI_GOTOPOS, f.editor.send(SCI_POSITIONFROMLINE, 1));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    // Should have fold markers in the margin
    ok &= check(!frame.marker_primitives.empty(), id, "fold margin with fold levels must produce marker_primitives");

    // Verify fold_part is carried through for at least some markers
    bool has_nonzero_fold_part = false;
    bool has_selected_background = false;
    for (const auto& m : frame.marker_primitives) {
        if (m.fold_part > 0) {
            has_nonzero_fold_part = true;
        }
        if (m.background_selected.isValid() && m.background_selected.alpha() > 0) {
            has_selected_background = true;
        }
        // fold_part must be in valid range [0..4]
        ok &= check(m.fold_part >= 0 && m.fold_part <= 4, id, "marker fold_part must be in range [0..4]");
    }
    ok &= check(has_nonzero_fold_part, id, "at least one marker must have non-zero fold_part");
    ok &= check(has_selected_background, id, "highlighted fold markers must preserve selected background color");

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 8: annotation with boxed outline
// ---------------------------------------------------------------------------

static bool test_annotation_boxed()
{
    const char* id = "annotation_boxed";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three");

    const sptr_t annotation_style_offset = f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_ANNOTATIONSETSTYLEOFFSET, annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, annotation_style_offset, 0xC2F4FF);

    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED, 0);
    f.editor.send(SCI_ANNOTATIONSETSTYLE, 0, 0);
    const char* annot_text = "boxed annotation text";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id, "boxed annotation must produce annotation primitives");
    if (!frame.annotations.empty()) {
        const Annotation_primitive& annot = frame.annotations[0];
        ok &= check(!annot.text.isEmpty(),      id, "annotation text must be non-empty");
        ok &= check(annot.rect.width()  > 0,    id, "annotation rect width must be > 0");
        ok &= check(annot.rect.height() > 0,    id, "annotation rect height must be > 0");
        ok &= check(annot.document_line == 0,   id, "annotation must be on the correct line");
        ok &= check(annot.boxed,                id, "annotation must have boxed=true for ANNOTATION_BOXED style");
        ok &= check(annot.foreground.isValid(), id, "annotation must have valid foreground color");
        ok &= check(annot.background.isValid(), id, "annotation must have valid background color");
        ok &= check(annot.foreground != annot.background, id,
            "annotation boxed outline must have visible foreground/background contrast");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 9: BiDi direction fidelity
// ---------------------------------------------------------------------------

static bool test_ltr_direction_field()
{
    const char* id = "ltr_direction_field";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("Hello World");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "text must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const Visual_line_frame& line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id, "text must produce text runs");
        for (const Text_run& run : line.text_runs) {
            ok &= check(run.direction == Text_direction::left_to_right, id,
                "Latin text runs must have left_to_right direction");
        }
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

static bool test_rtl_direction_field()
{
    const char* id = "utf8_hebrew_capture";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    // Hebrew text without BiDi mode: verifies UTF-8 Hebrew survives capture
    // and produces sane run geometry. This is not a full RTL layout test.
    f.set_text("A" "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d" "B");
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "text with Hebrew must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const Visual_line_frame& line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id, "text with Hebrew must produce text runs");
        for (const Text_run& run : line.text_runs) {
            ok &= check(run.width > 0 || run.text.isEmpty(), id,
                "all non-empty runs must have positive width");
        }
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

static bool test_mixed_bidi_direction()
{
    const char* id = "mixed_utf8_capture";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    // Mixed LTR+RTL without BiDi mode: verifies UTF-8 mixed text capture
    // and run geometry only. This is not a mixed-direction layout fidelity test.
    f.set_text("Hello " "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d" " World");
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "mixed BiDi text must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const Visual_line_frame& line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id, "mixed BiDi text must produce text runs");

        for (const Text_run& run : line.text_runs) {
            ok &= check(run.width > 0 || run.text.isEmpty(), id, "all non-empty runs must have positive width");
        }

        QString full_text = reconstruct_line_text(line);
        ok &= check(full_text.contains("Hello"), id, "mixed text must contain Latin portion");
        ok &= check(full_text.contains("World"), id, "mixed text must contain trailing Latin portion");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

static bool test_annotation_boxed_padding()
{
    const char* id = "annotation_boxed_padding";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three\n");
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED);
    f.editor.send(SCI_ANNOTATIONSETSTYLE, 1, STYLE_DEFAULT);
    const char* annot_text = "boxed annotation with padding";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 1, reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id, "boxed annotation must produce annotation primitives");

    for (const Annotation_primitive& annot : frame.annotations) {
        ok &= check(annot.boxed, id, "annotation must have boxed flag set");
        ok &= check(annot.rect.width()  > 0,                   id, "boxed annotation rect width must be > 0");
        ok &= check(annot.rect.height() > 0,                   id, "boxed annotation rect height must be > 0");
        ok &= check(annot.position.x() >= annot.rect.left(),   id, "annotation text X must be >= rect left (padding)");
        ok &= check(annot.position.x() <  annot.rect.right(),  id, "annotation text X must be < rect right");
        ok &= check(annot.baseline_y   >= annot.rect.top(),    id, "annotation baseline must be >= rect top");
        ok &= check(annot.baseline_y   <= annot.rect.bottom(), id, "annotation baseline must be <= rect bottom");
    }

    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 9: EOL annotation boxed (visible_style fidelity)
// ---------------------------------------------------------------------------

static bool test_eol_annotation_boxed()
{
    const char* id = "eol_annotation_boxed";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\n");
    const sptr_t eol_annotation_style_offset = f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLEOFFSET, eol_annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, eol_annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, eol_annotation_style_offset, 0xC2F4FF);
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_BOXED);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLE, 0, 0);
    const char* annot_text = "boxed eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0, reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.eol_annotations.empty(), id, "boxed EOL annotation must produce eol_annotations");
    if (!frame.eol_annotations.empty()) {
        const Eol_annotation_primitive& eol = frame.eol_annotations[0];
        ok &= check(eol.text == QString::fromUtf8(annot_text), id,
            "boxed EOL annotation text must match");
        ok &= check(eol.rect.width() > 0, id,
            "boxed EOL annotation rect width must be > 0");
        ok &= check(eol.rect.height() > 0, id,
            "boxed EOL annotation rect height must be > 0");
        ok &= check(eol.document_line == 0, id,
            "boxed EOL annotation must be on document line 0");
        ok &= check(eol.visible_style == EOLANNOTATION_BOXED, id,
            "visible_style must be EOLANNOTATION_BOXED");
        ok &= check(eol.foreground.isValid(), id,
            "boxed EOL annotation foreground must be valid");
        ok &= check(eol.background.isValid(), id,
            "boxed EOL annotation background must be valid");
        ok &= check(eol.foreground != eol.background, id,
            "boxed EOL annotation must have visible foreground/background contrast");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 9: overlapping indicators
// ---------------------------------------------------------------------------

static bool test_overlapping_indicators()
{
    const char* id = "overlapping_indicators";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("Hello World Test");
    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETSTYLE, 1, INDIC_BOX);
    f.editor.send(SCI_INDICSETFORE, 1, 0xFF0000);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 0, 11);
    f.editor.send(SCI_SETINDICATORCURRENT, 1);
    f.editor.send(SCI_INDICATORFILLRANGE, 6, 10);
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.indicator_primitives.size() >= 2, id, "overlapping indicators must produce multiple primitives");

    bool has_squiggle = false;
    bool has_box = false;
    for (const Indicator_primitive& ind : frame.indicator_primitives) {
        if (ind.indicator_style == INDIC_SQUIGGLE) {
            has_squiggle = true;
        }
        if (ind.indicator_style == INDIC_BOX) {
            has_box = true;
        }
    }
    ok &= check(has_squiggle, id, "must have squiggle indicator");
    ok &= check(has_box, id, "must have box indicator");

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_indicator_within_text_area(frame, id);
    ok &= check_indicator_overlaps_visual_line(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void stderrMessageHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    fprintf(stderr, "%s\n", qPrintable(msg));
    fflush(stderr);
}

int main(int argc, char** argv)
{
    qInstallMessageHandler(stderrMessageHandler);

    // Pin DPI and scale factor for deterministic metrics even when run
    // outside of CTest (which sets these in the environment).
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qCritical("%s", qPrintable(font_error));
        return 1;
    }

    // Verify the test font is actually available.  Silent substitution
    // would invalidate every width/wrap/overlap assertion.
    {
        const QString family = scintillaquick::shared::deterministic_test_font_family();
        QFont probe(family, 11);
        QFontInfo info(probe);
        if (info.family().compare(family, Qt::CaseInsensitive) != 0) {
            qCritical("FAIL: bundled test font not available (resolved to '%s'). "
                    "Frame validation requires the configured bundled font for deterministic metrics.",
                qPrintable(info.family()));
            return 1;
        }
    }

    qDebug("=== Frame Validation (phases 2-9, structural + semantic) ===\n");

    struct
    {
        const char* name;
        bool (*fn)();
    }
    fixtures[] = {

        // Phase 2 fixtures
        {"plain_ascii_short",                        test_plain_ascii_short},
        {"plain_ascii_long_wrap",                    test_plain_ascii_long_wrap},
        {"cached_frame_wrap_toggle_rebuilds",        test_cached_frame_wrap_toggle_rebuilds},
        {"horizontal_scroll_resets_on_doc_switch",   test_horizontal_scroll_resets_on_doc_switch},
        {"caret_left_scrolls_to_long_previous_line", test_caret_left_scrolls_to_long_previous_line},
        {"cached_overlay_only_selection_refreshes_overlay",
                                                    test_cached_overlay_only_selection_refreshes_overlay},
        {"scrolled_full_capture_matches_direct_secondary_geometry",
                                                    test_scrolled_full_capture_matches_direct_secondary_geometry},
        {"edit_savepoint_one_line_scroll_matches_fresh_body_text",
                                                    test_edit_savepoint_one_line_scroll_matches_fresh_body_text},
        {"mixed_styles_wrap",                        test_mixed_styles_wrap},
        {"tab_layout_default",                       test_tab_layout_default},
        {"tab_layout_nondefault",                    test_tab_layout_nondefault},
        {"selection_single_line",                    test_selection_single_line},
        {"selection_wrap_boundary",                  test_selection_wrap_boundary},
        {"caret_mid_line",                           test_caret_mid_line},
        {"caret_wrap_continuation",                  test_caret_wrap_continuation},
        {"margin_numbers_basic",                     test_margin_numbers_basic},
        {"margin_numbers_wrap",                      test_margin_numbers_wrap},
        {"plain_indicator_basic",                    test_plain_indicator_basic},

        // Phase 3: multi-selection, rectangular selection, current-line
        {"multi_selection",                          test_multi_selection},
        {"rectangular_selection",                    test_rectangular_selection},
        {"current_line_frame",                       test_current_line_frame},

        // Phase 4: expanded indicator styles
        {"squiggle_indicator",                       test_squiggle_indicator},
        {"box_indicator",                            test_box_indicator},

        // Phase 5: marker symbol
        {"marker_symbol",                            test_marker_symbol},

        // Phase 6: multi-caret
        {"multi_caret",                              test_multi_caret},

        // Phase 7: new visual families
        {"whitespace_visible",                       test_whitespace_visible},
        {"eol_annotation",                           test_eol_annotation},
        {"annotation",                               test_annotation},
        {"indent_guide",                             test_indent_guide},
        {"style_underline",                          test_style_underline},

        // Phase 8: fold/marker/annotation fidelity
        {"fold_display_text",                        test_fold_display_text},
        {"marker_fold_part",                         test_marker_fold_part},
        {"annotation_boxed",                         test_annotation_boxed},

        // Phase 9: UTF-8 text-run capture, boxed EOL annotations, overlapping indicators
        {"ltr_direction_field",                      test_ltr_direction_field},
        {"utf8_hebrew_capture",                      test_rtl_direction_field},
        {"mixed_utf8_capture",                       test_mixed_bidi_direction},
        {"annotation_boxed_padding",                 test_annotation_boxed_padding},
        {"eol_annotation_boxed",                     test_eol_annotation_boxed},
        {"overlapping_indicators",                   test_overlapping_indicators},
    };

    int fixture_pass = 0;
    int fixture_fail = 0;

    for (const auto& f : fixtures) {
        bool ok = f.fn();
        if (ok) {
            qDebug("  PASS [%s]\n", f.name);
            ++fixture_pass;
        }
        else {
            qWarning("  FIXTURE FAILED [%s]\n", f.name);
            ++fixture_fail;
        }
    }

    qDebug("=== Results: %d/%d fixtures passed, %d/%d individual checks passed ===", fixture_pass,
        fixture_pass + fixture_fail, g_pass_count, g_pass_count + g_fail_count);

    if (fixture_fail > 0) {
        qDebug("NOTE: Failures indicate the frame capture pipeline does not yet "
               "produce expected primitives for these corpus cases.");
    }

    return fixture_fail > 0 ? 1 : 0;
}
