#include "scintillaquick_validation_access.h"

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QFontInfo>
#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
#include <vector>

#include "Scintilla.h"

using namespace Scintilla::Internal;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

int g_pass_count = 0;
int g_fail_count = 0;

struct Fixture_editor
{
    ScintillaQuickItem editor;

    Fixture_editor()
    {
        editor.setWidth(640);
        editor.setHeight(480);
        editor.setProperty("font", QFont(QStringLiteral("Consolas"), 11));
        // Make caret visible for capture: give focus and disable blink.
        editor.send(SCI_SETFOCUS, 1);
        editor.send(SCI_SETCARETPERIOD, 0);
    }

    void set_text(const char *text)
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
    ScintillaQuickItem &editor,
    int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(&editor, &event);
}

bool check(bool condition, const char *fixture_id, const char *detail)
{
    if (!condition) {
        qWarning("  FAIL [%s] %s", fixture_id, detail);
        ++g_fail_count;
        return false;
    }
    ++g_pass_count;
    return true;
}

QString reconstruct_line_text(const visual_line_frame_t &line)
{
    QString result;
    for (const text_run_t &run : line.text_runs) {
        result += run.text;
    }
    return result;
}

const visual_line_frame_t *find_visual_line(const Render_frame &frame, int document_line)
{
    for (const visual_line_frame_t &line : frame.visual_lines) {
        if (line.key.document_line == document_line && line.key.subline_index == 0) {
            return &line;
        }
    }

    return nullptr;
}

const text_run_t *first_non_empty_run(const visual_line_frame_t *line)
{
    if (!line) {
        return nullptr;
    }

    for (const text_run_t &run : line->text_runs) {
        if (!run.text.isEmpty()) {
            return &run;
        }
    }

    return nullptr;
}

void dump_frame_summary(const char *fixture_id, const Render_frame &frame)
{
    qDebug("  [%s] visual_lines=%lld  selections=%lld  carets=%lld  "
           "indicators=%lld  current_lines=%lld  markers=%lld  margin_texts=%lld  "
           "fold_texts=%lld  eol_annots=%lld  annots=%lld  ws_marks=%lld  "
           "decor_ul=%lld  indent_guides=%lld",
           fixture_id,
           static_cast<long long>(frame.visual_lines.size()),
           static_cast<long long>(frame.selection_primitives.size()),
           static_cast<long long>(frame.caret_primitives.size()),
           static_cast<long long>(frame.indicator_primitives.size()),
           static_cast<long long>(frame.current_line_primitives.size()),
           static_cast<long long>(frame.marker_primitives.size()),
           static_cast<long long>(frame.margin_text_primitives.size()),
           static_cast<long long>(frame.fold_display_texts.size()),
           static_cast<long long>(frame.eol_annotations.size()),
           static_cast<long long>(frame.annotations.size()),
           static_cast<long long>(frame.whitespace_marks.size()),
           static_cast<long long>(frame.decoration_underlines.size()),
           static_cast<long long>(frame.indent_guides.size()));
}

// ---------------------------------------------------------------------------
// Semantic geometry helpers
// ---------------------------------------------------------------------------

// Check that no two text runs within any visual line overlap horizontally.
// Overlapping runs indicate stacked/garbled glyph output -- the exact
// regression that motivated this validation tranche.
bool check_no_overlapping_runs(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &vl : frame.visual_lines) {
        for (size_t i = 0; i < vl.text_runs.size(); ++i) {
            double a_left = vl.text_runs[i].position.x();
            double a_right = a_left + vl.text_runs[i].width;
            for (size_t j = i + 1; j < vl.text_runs.size(); ++j) {
                double b_left = vl.text_runs[j].position.x();
                double b_right = b_left + vl.text_runs[j].width;
                double overlap = std::min(a_right, b_right)
                               - std::max(a_left, b_left);
                ok &= check(overlap <= 0.1, id,
                    "text runs overlap horizontally (stacked/garbled glyph risk)");
            }
        }
    }
    return ok;
}

// Check that every non-empty text run has positive width.
// Zero-width runs mean invisible text.
bool check_runs_positive_width(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &vl : frame.visual_lines) {
        for (const auto &run : vl.text_runs) {
            if (!run.text.isEmpty()) {
                ok &= check(run.width > 0.0, id,
                    "non-empty text run has zero or negative width");
            }
        }
    }
    return ok;
}

// Check that text runs start within (or very near) their visual line's
// clip_rect horizontal bounds.
bool check_runs_within_line_clip(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &vl : frame.visual_lines) {
        if (!vl.clip_rect.isValid()) continue;
        for (const auto &run : vl.text_runs) {
            ok &= check(run.position.x() >= vl.clip_rect.left() - 2.0, id,
                "text run starts before visual line clip_rect left edge");
        }
    }
    return ok;
}

// For wrapped text: sublines of the same document line must have
// monotonically increasing Y positions matching their subline_index order.
bool check_sublines_ordered_by_y(const Render_frame &frame, const char *id)
{
    std::map<int, std::vector<const visual_line_frame_t *>> by_doc_line;
    for (const auto &vl : frame.visual_lines) {
        by_doc_line[vl.key.document_line].push_back(&vl);
    }

    bool ok = true;
    for (const auto &[doc_line, lines] : by_doc_line) {
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
bool check_selection_overlaps_visual_line(
    const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &sel : frame.selection_primitives) {
        bool overlaps_any = false;
        for (const auto &vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) continue;
            if (sel.rect.bottom() > vl.clip_rect.top() &&
                sel.rect.top() < vl.clip_rect.bottom()) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id,
            "selection rect does not vertically overlap any visual line");
    }
    return ok;
}

// Each caret rectangle should vertically overlap at least one visual line.
bool check_caret_within_line_bounds(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &caret : frame.caret_primitives) {
        bool overlaps_any = false;
        for (const auto &vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) continue;
            if (caret.rect.bottom() > vl.clip_rect.top() - 1.0 &&
                caret.rect.top() < vl.clip_rect.bottom() + 1.0) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id,
            "caret rect does not vertically overlap any visual line");
    }
    return ok;
}

// Phase 2 contract: margin primitives must appear on the first wrapped
// subline only (subline_index == 0).
bool check_margin_first_subline_only(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &mt : frame.margin_text_primitives) {
        ok &= check(mt.subline_index == 0, id,
            "margin text primitive has subline_index != 0 "
            "(must be first subline only)");
    }
    return ok;
}

// Margin text top-Y should be close to the corresponding body visual line
// top-Y (within a small tolerance for baseline adjustments).
bool check_margin_y_aligns_with_body(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &mt : frame.margin_text_primitives) {
        bool found = false;
        for (const auto &vl : frame.visual_lines) {
            if (vl.key.document_line == mt.document_line &&
                vl.key.subline_index == 0) {
                double y_diff = std::abs(mt.clip_rect.top() - vl.clip_rect.top());
                ok &= check(y_diff < 2.0, id,
                    "margin text Y not aligned with body visual line Y "
                    "(> 2px drift)");
                found = true;
                break;
            }
        }
        ok &= check(found, id,
            "margin text has no matching body visual line for its doc line");
    }
    return ok;
}

// Indicator rectangles should fall within the text area bounds.
bool check_indicator_within_text_area(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &ind : frame.indicator_primitives) {
        ok &= check(ind.rect.left() >= frame.text_rect.left() - 2.0, id,
            "indicator rect extends left of text area");
        ok &= check(ind.rect.right() <= frame.text_rect.right() + 2.0, id,
            "indicator rect extends right of text area");
    }
    return ok;
}

// Each indicator rectangle should vertically overlap at least one visual line.
bool check_indicator_overlaps_visual_line(
    const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &ind : frame.indicator_primitives) {
        bool overlaps_any = false;
        for (const auto &vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) continue;
            if (ind.rect.bottom() > vl.clip_rect.top() - 2.0 &&
                ind.rect.top() < vl.clip_rect.bottom() + 2.0 &&
                ind.rect.right() > vl.clip_rect.left() - 2.0 &&
                ind.rect.left() < vl.clip_rect.right() + 2.0) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id,
            "indicator rect does not overlap any visual line area");
    }
    return ok;
}

// For LTR text, text runs within a visual line should appear in
// monotonically increasing X order.
bool check_runs_x_ordered(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &vl : frame.visual_lines) {
        for (size_t i = 0; i + 1 < vl.text_runs.size(); ++i) {
            if (vl.text_runs[i].direction == text_direction_t::left_to_right &&
                vl.text_runs[i + 1].direction == text_direction_t::left_to_right) {
                ok &= check(
                    vl.text_runs[i + 1].position.x() >=
                        vl.text_runs[i].position.x() - 0.1,
                    id,
                    "LTR text runs not in left-to-right X order");
            }
        }
    }
    return ok;
}

// Check that no two visual lines have vertically overlapping clip_rects.
// Overlapping clip_rects mean text from one line paints over another.
bool check_visual_lines_no_vertical_overlap(
    const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (size_t i = 0; i < frame.visual_lines.size(); ++i) {
        const auto &a = frame.visual_lines[i];
        if (!a.clip_rect.isValid()) continue;
        for (size_t j = i + 1; j < frame.visual_lines.size(); ++j) {
            const auto &b = frame.visual_lines[j];
            if (!b.clip_rect.isValid()) continue;
            double v_overlap = std::min(a.clip_rect.bottom(), b.clip_rect.bottom())
                             - std::max(a.clip_rect.top(), b.clip_rect.top());
            ok &= check(v_overlap <= 0.1, id,
                "visual lines have vertically overlapping clip rects "
                "(text from different lines would overlap)");
        }
    }
    return ok;
}

constexpr double k_geometry_tolerance = 1.5;

QColor scintilla_iprgb_color(int value)
{
    return QColor(
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff);
}

bool nearly_equal(double a, double b, double tolerance = k_geometry_tolerance)
{
    return std::abs(a - b) <= tolerance;
}

int count_visual_lines_for_document_line(const Render_frame &frame, int document_line)
{
    return static_cast<int>(std::count_if(
        frame.visual_lines.begin(),
        frame.visual_lines.end(),
        [document_line](const visual_line_frame_t &line) {
            return line.key.document_line == document_line;
        }));
}

const visual_line_frame_t *find_matching_visual_line(
    const Render_frame &frame,
    const QRectF &rect,
    int document_line = -1)
{
    const visual_line_frame_t *best = nullptr;
    double best_distance = std::numeric_limits<double>::max();

    for (const auto &line : frame.visual_lines) {
        if (!line.clip_rect.isValid()) {
            continue;
        }
        if (document_line >= 0 && line.key.document_line != document_line) {
            continue;
        }
        if (rect.bottom() < line.clip_rect.top() - k_geometry_tolerance ||
            rect.top() > line.clip_rect.bottom() + k_geometry_tolerance) {
            continue;
        }

        const double distance =
            std::abs(rect.top() - line.clip_rect.top()) +
            std::abs(rect.bottom() - line.clip_rect.bottom());
        if (distance < best_distance) {
            best = &line;
            best_distance = distance;
        }
    }

    return best;
}

// Selection rects should fall within the text area's horizontal bounds.
bool check_selection_within_text_area(const Render_frame &frame, const char *id)
{
    bool ok = true;
    for (const auto &sel : frame.selection_primitives) {
        ok &= check(sel.rect.left() >= frame.text_rect.left() - 2.0, id,
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
    const char *id = "plain_ascii_short";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("alpha beta gamma");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check(frame.visual_lines[0].key.document_line == 0, id,
                     "first line document_line must be 0");
        ok &= check(frame.visual_lines[0].key.subline_index == 0, id,
                     "first line subline_index must be 0");
        ok &= check(!frame.visual_lines[0].text_runs.empty(), id,
                     "first line must have text runs");
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
    const char *id = "plain_ascii_long_wrap";
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
    ok &= check(frame.visual_lines.size() >= 3, id,
                 "must produce >= 3 visual lines from wrap");

    int max_subline = 0;
    for (const visual_line_frame_t &vl : frame.visual_lines) {
        if (vl.key.document_line == 0 && vl.key.subline_index > max_subline) {
            max_subline = vl.key.subline_index;
        }
    }
    ok &= check(max_subline >= 2, id,
                 "must have subline_index >= 2 for doc line 0");

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_horizontal_scroll_resets_on_doc_switch()
{
    const char *id = "horizontal_scroll_resets_on_doc_switch";
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
    const int short_scroll_width = static_cast<int>(f.editor.send(SCI_GETSCROLLWIDTH));
    const int short_x_offset = static_cast<int>(f.editor.send(SCI_GETXOFFSET));

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
    const char *id = "caret_left_scrolls_to_long_previous_line";
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

    const text_run_t *cached_long_run = first_non_empty_run(find_visual_line(cached_after, 1));
    const text_run_t *direct_long_run = first_non_empty_run(find_visual_line(direct_after, 1));

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

static bool test_mixed_styles_wrap()
{
    const char *id = "mixed_styles_wrap";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    // Configure distinct styles.
    f.editor.send(SCI_STYLESETFONT,
                  STYLE_DEFAULT,
                  reinterpret_cast<sptr_t>("Consolas"));
    f.editor.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    f.editor.send(SCI_STYLECLEARALL);
    f.editor.send(SCI_STYLESETFORE, 1, 0x000000);
    f.editor.send(SCI_STYLESETBOLD, 1, 1);
    f.editor.send(SCI_STYLESETFORE, 2, 0x0000FF);
    f.editor.send(SCI_STYLESETITALIC, 2, 1);
    f.editor.send(SCI_STYLESETFORE, 3, 0xFF0000);

    const char *text =
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
    ok &= check(frame.visual_lines.size() >= 2, id,
                 "styled text must wrap into >= 2 visual lines");

    // Check that at least one visual line has runs with different style_ids.
    bool found_multi_style_line = false;
    for (const visual_line_frame_t &vl : frame.visual_lines) {
        if (vl.text_runs.size() >= 2) {
            int first_style = vl.text_runs[0].style_id;
            for (size_t i = 1; i < vl.text_runs.size(); ++i) {
                if (vl.text_runs[i].style_id != first_style) {
                    found_multi_style_line = true;
                    break;
                }
            }
        }
        if (found_multi_style_line) break;
    }
    ok &= check(found_multi_style_line, id,
                 "at least one visual line must have runs with different style_ids");

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_sublines_ordered_by_y(frame, id);
    return ok;
}

static bool test_tab_layout_default()
{
    const char *id = "tab_layout_default";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETTABWIDTH, 8);
    f.set_text("col1\tcol2\tcol3");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check(!frame.visual_lines[0].text_runs.empty(), id,
                     "first line must have text runs");
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
    const char *id = "tab_layout_nondefault";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETTABWIDTH, 4);
    f.set_text("col1\tcol2\tcol3");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id, "visual_lines must not be empty");
    if (!frame.visual_lines.empty()) {
        ok &= check(!frame.visual_lines[0].text_runs.empty(), id,
                     "first line must have text runs");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_runs_positive_width(frame, id);
    return ok;
}

static bool test_selection_single_line()
{
    const char *id = "selection_single_line";
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
        const Selection_primitive &sel = frame.selection_primitives[0];
        const int selection_start = 7;
        const int selection_end = 11;
        const int selection_line = static_cast<int>(f.editor.send(SCI_LINEFROMPOSITION, selection_start));
        const double expected_left = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, selection_start));
        const double expected_right = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, selection_end));
        const double expected_top = static_cast<double>(
            f.editor.send(SCI_POINTYFROMPOSITION, 0, f.editor.send(SCI_POSITIONFROMLINE, selection_line)));
        const double expected_bottom = expected_top + static_cast<double>(f.editor.send(SCI_TEXTHEIGHT, selection_line));
        const visual_line_frame_t *line = find_matching_visual_line(frame, sel.rect, selection_line);

        ok &= check(sel.is_main, id, "single-line selection must be marked as main");
        ok &= check(sel.rect.width() > 0, id, "selection rect width must be > 0");
        ok &= check(sel.rect.height() > 0, id, "selection rect height must be > 0");
        ok &= check(line != nullptr, id, "selection rect must map to a visual line");
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
    const char *id = "selection_wrap_boundary";
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
        for (const auto &sel : frame.selection_primitives) {
            const visual_line_frame_t *line = find_matching_visual_line(frame, sel.rect);
            ok &= check(sel.rect.width() > 0, id,
                         "wrap-spanning selection rect width must be > 0");
            ok &= check(sel.rect.height() > 0, id,
                         "wrap-spanning selection rect height must be > 0");
            ok &= check(line != nullptr, id,
                         "wrap-spanning selection rect must map to a visual line");
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
    const char *id = "caret_mid_line";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("caret goes here in the middle of styled text");
    // Place caret at position 16 (no selection).
    f.editor.send(SCI_GOTOPOS, 16);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.caret_primitives.empty(), id,
                 "caret_primitives must not be empty");
    if (!frame.caret_primitives.empty()) {
        const caret_primitive_t &caret = frame.caret_primitives[0];
        ok &= check(caret.rect.height() > 0, id, "caret rect height must be > 0");
        // Caret should be within text bounds.
        ok &= check(caret.rect.left() >= frame.text_rect.left(), id,
                     "caret left must be >= text_rect left");
    }

    // Semantic geometry checks.
    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_caret_within_line_bounds(frame, id);
    return ok;
}

static bool test_caret_wrap_continuation()
{
    const char *id = "caret_wrap_continuation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.setWidth(200);
    f.editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);

    const char *text =
        "The quick brown fox jumps over the lazy dog and then continues "
        "running around the field to test caret on a wrapped continuation.";
    f.set_text(text);

    // Move caret to the latter part (should be on a continuation subline).
    int text_len = static_cast<int>(strlen(text));
    f.editor.send(SCI_GOTOPOS, text_len - 10);

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.caret_primitives.empty(), id,
                 "caret_primitives must not be empty on continuation");
    if (!frame.caret_primitives.empty()) {
        const caret_primitive_t &caret = frame.caret_primitives[0];
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
    const char *id = "margin_numbers_basic";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    f.editor.send(SCI_SETMARGINWIDTHN, 0, 40);
    f.set_text("line one\nline two\nline three\nline four\nline five");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.margin_text_primitives.empty(), id,
                 "margin_text_primitives must not be empty with line numbers on");
    ok &= check(frame.margin_text_primitives.size() == 5, id,
                 "numbered margin must produce exactly one primitive per document line");

    // Check that margin primitives contain expected line-number text.
    bool found_1 = false, found_2 = false;
    for (const Margin_text_primitive &m : frame.margin_text_primitives) {
        QString trimmed = m.text.trimmed();
        if (trimmed == "1") found_1 = true;
        if (trimmed == "2") found_2 = true;
        ok &= check(m.clip_rect.width() > 0, id,
                     "margin clip_rect width must be > 0");
        ok &= check(m.clip_rect.height() > 0, id,
                     "margin clip_rect height must be > 0");
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
    const char *id = "margin_numbers_wrap";
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
    for (const Margin_text_primitive &m : frame.margin_text_primitives) {
        doc_line_count[m.document_line]++;
    }
    for (const auto &[doc_line, count] : doc_line_count) {
        if (count > 1) {
            qWarning("  [%s] document_line %d has %d margin primitives "
                     "(expected 1 per doc line)", id, doc_line, count);
        }
        ok &= check(count == 1, id,
                     "each document line should have exactly one margin primitive");
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
    const char *id = "plain_indicator_basic";
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
        const Indicator_primitive &ind = frame.indicator_primitives[0];
        const int indicator_start = 13;
        const int indicator_end = 22;
        const int indicator_line = static_cast<int>(f.editor.send(SCI_LINEFROMPOSITION, indicator_start));
        const double expected_left = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, indicator_start));
        const double expected_right = static_cast<double>(f.editor.send(SCI_POINTXFROMPOSITION, 0, indicator_end));
        const double expected_top = static_cast<double>(
            f.editor.send(SCI_POINTYFROMPOSITION, 0, f.editor.send(SCI_POSITIONFROMLINE, indicator_line)));
        const double expected_bottom = expected_top + static_cast<double>(f.editor.send(SCI_TEXTHEIGHT, indicator_line));
        const visual_line_frame_t *line = find_matching_visual_line(frame, ind.rect, indicator_line);

        ok &= check(ind.rect.width() > 0, id, "indicator rect width must be > 0");
        ok &= check(ind.rect.height() > 0, id, "indicator rect height must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0x0000FF), id,
                     "indicator color must match the configured indicator color");
        ok &= check(ind.indicator_number == 0, id, "indicator number must be 0");
        ok &= check(ind.indicator_style == static_cast<int>(INDIC_PLAIN), id,
                     "indicator style must be plain");
        ok &= check(line != nullptr, id, "indicator rect must map to a visual line");
        ok &= check(line && line->key.document_line == indicator_line, id,
                     "indicator rect must map to the expected document line");
        ok &= check(nearly_equal(ind.rect.left(), expected_left), id,
                     "indicator rect left edge must match the indicator start");
        ok &= check(nearly_equal(ind.rect.right(), expected_right), id,
                     "indicator rect right edge must match the indicator end");
        ok &= check(ind.rect.top() >= expected_top - k_geometry_tolerance, id,
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
    const char *id = "multi_selection";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("aaa bbb ccc ddd eee fff");
    // Add three separate selections (multi-select).
    f.editor.send(SCI_SETSELECTION, 0, 3);       // "aaa"
    f.editor.send(SCI_ADDSELECTION, 4, 7);        // "bbb"
    f.editor.send(SCI_ADDSELECTION, 8, 11);       // "ccc"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.selection_primitives.size() == 3, id,
                 "must have exactly 3 selection primitives for 3 selections");

    // Check that exactly one selection is marked as main and that all three
    // selections are on the same line but do not overlap horizontally.
    int main_count = 0;
    std::vector<QRectF> rects;
    for (const auto &sel : frame.selection_primitives) {
        ok &= check(sel.rect.width() > 0, id, "selection rect width must be > 0");
        ok &= check(sel.rect.height() > 0, id, "selection rect height must be > 0");
        ok &= check(sel.rect.top() == frame.selection_primitives.front().rect.top() ||
                    nearly_equal(sel.rect.top(), frame.selection_primitives.front().rect.top()), id,
                    "multi-selection rects must sit on the same visual line");
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
    const char *id = "rectangular_selection";
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
    ok &= check(frame.selection_primitives.size() == 3, id,
                 "rectangular selection must produce exactly three primitives");
    if (!frame.selection_primitives.empty()) {
        const double reference_left = frame.selection_primitives.front().rect.left();
        const double reference_right = frame.selection_primitives.front().rect.right();
        std::map<int, int> document_line_counts;
        int main_count = 0;

        for (const auto &sel : frame.selection_primitives) {
            const visual_line_frame_t *line = find_matching_visual_line(frame, sel.rect);
            ok &= check(sel.rect.width() > 0, id, "rectangular selection rect width must be > 0");
            ok &= check(sel.rect.height() > 0, id, "rectangular selection rect height must be > 0");
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

        ok &= check(main_count == 1, id, "rectangular selection must have exactly one main primitive");
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
    const char *id = "current_line_frame";
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
    const int caret_position = static_cast<int>(f.editor.send(SCI_GETCURRENTPOS));
    const int caret_line = static_cast<int>(f.editor.send(SCI_LINEFROMPOSITION, caret_position));
    const int expected_visual_line_count = count_visual_lines_for_document_line(frame, caret_line);

    ok &= check(frame.current_line_primitives.size() == static_cast<size_t>(expected_visual_line_count), id,
                 "current-line highlight must produce one primitive per visual subline");
    ok &= check(expected_visual_line_count >= 1, id,
                 "caret line must exist in the captured frame");
    if (!frame.current_line_primitives.empty()) {
        for (const auto &cl : frame.current_line_primitives) {
            const visual_line_frame_t *line = find_matching_visual_line(frame, cl.rect, caret_line);
            ok &= check(cl.rect.width() > 0, id, "current line rect width must be > 0");
            ok &= check(cl.rect.height() > 0, id, "current line rect height must be > 0");
            ok &= check(cl.color == scintilla_iprgb_color(0xFFFFE0), id,
                         "current line color must match the configured caret-line color");
            ok &= check(cl.framed, id,
                         "current line primitive must preserve framed caret-line styling");
            ok &= check(nearly_equal(cl.rect.left(), frame.text_rect.left()), id,
                         "current line rect must start at the text area left edge");
            ok &= check(nearly_equal(cl.rect.right(), frame.text_rect.right()), id,
                         "current line rect must end at the text area right edge");
            ok &= check(line != nullptr, id, "current line rect must map to a visual line");
            ok &= check(line && line->key.document_line == caret_line, id,
                         "current line rect must map to the caret document line");
        }
    }

    // Current line should overlap a visual line.
    for (const auto &cl : frame.current_line_primitives) {
        bool overlaps_any = false;
        for (const auto &vl : frame.visual_lines) {
            if (!vl.clip_rect.isValid()) continue;
            if (cl.rect.bottom() > vl.clip_rect.top() &&
                cl.rect.top() < vl.clip_rect.bottom()) {
                overlaps_any = true;
                break;
            }
        }
        ok &= check(overlaps_any, id,
            "current line rect must overlap at least one visual line");
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
    const char *id = "squiggle_indicator";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("error on this word here");

    f.editor.send(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
    f.editor.send(SCI_INDICSETFORE, 0, 0x0000FF);
    f.editor.send(SCI_INDICSETUNDER, 0, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 0);
    f.editor.send(SCI_INDICATORFILLRANGE, 9, 4);  // "this"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.indicator_primitives.size() == 1, id,
                 "squiggle indicator test must produce exactly one indicator primitive");
    if (!frame.indicator_primitives.empty()) {
        const Indicator_primitive &ind = frame.indicator_primitives[0];
        const visual_line_frame_t *line = find_matching_visual_line(frame, ind.rect, 0);
        ok &= check(ind.rect.width() > 0, id, "squiggle indicator rect width must be > 0");
        ok &= check(ind.rect.height() > 0, id, "squiggle indicator rect height must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0x0000FF), id,
                     "squiggle indicator color must match the configured indicator color");
        ok &= check(ind.indicator_style == static_cast<int>(INDIC_SQUIGGLE), id,
            "indicator_style must be INDIC_SQUIGGLE");
        ok &= check(ind.indicator_number == 0, id,
            "indicator_number must be 0");
        ok &= check(ind.under_text, id,
            "squiggle indicator must stay in the under-text layer");
        ok &= check(line != nullptr, id, "squiggle indicator rect must map to a visual line");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    ok &= check_indicator_within_text_area(frame, id);
    ok &= check_indicator_overlaps_visual_line(frame, id);
    return ok;
}

static bool test_box_indicator()
{
    const char *id = "box_indicator";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("box around this word");

    f.editor.send(SCI_INDICSETSTYLE, 1, INDIC_BOX);
    f.editor.send(SCI_INDICSETFORE, 1, 0xFF0000);
    f.editor.send(SCI_INDICSETUNDER, 1, 1);
    f.editor.send(SCI_SETINDICATORCURRENT, 1);
    f.editor.send(SCI_INDICATORFILLRANGE, 4, 6);  // "around"

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(frame.indicator_primitives.size() == 1, id,
                 "box indicator test must produce exactly one indicator primitive");
    if (!frame.indicator_primitives.empty()) {
        const Indicator_primitive &ind = frame.indicator_primitives[0];
        const visual_line_frame_t *line = find_matching_visual_line(frame, ind.rect, 0);
        ok &= check(ind.rect.width() > 0, id, "box indicator rect width must be > 0");
        ok &= check(ind.color == scintilla_iprgb_color(0xFF0000), id,
                     "box indicator color must match the configured indicator color");
        ok &= check(ind.indicator_style == static_cast<int>(INDIC_BOX), id,
            "indicator_style must be INDIC_BOX");
        ok &= check(ind.indicator_number == 1, id,
            "indicator_number must be 1");
        ok &= check(ind.under_text, id,
            "box indicator must stay in the under-text layer");
        ok &= check(line != nullptr, id, "box indicator rect must map to a visual line");
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
    const char *id = "marker_symbol";
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
    ok &= check(frame.marker_primitives.size() == 1, id,
                 "marker fixture must produce exactly one marker primitive");
    if (!frame.marker_primitives.empty()) {
        const Marker_primitive &m = frame.marker_primitives[0];
        const visual_line_frame_t *line = find_matching_visual_line(frame, m.rect, 0);
        const double expected_top = line ? line->clip_rect.top() : 0.0;
        const double expected_bottom = line ? line->clip_rect.bottom() : 0.0;
        ok &= check(m.rect.width() > 0, id, "marker rect width must be > 0");
        ok &= check(m.rect.height() > 0, id, "marker rect height must be > 0");
        ok &= check(m.marker_number == 0, id, "marker_number must be 0");
        ok &= check(m.marker_type == static_cast<int>(SC_MARK_CIRCLE), id,
                     "marker_type must be SC_MARK_CIRCLE");
        ok &= check(m.document_line == 0, id, "marker must be on document line 0");
        ok &= check(m.foreground == scintilla_iprgb_color(0x000000), id,
                     "marker foreground color must match the configured marker color");
        ok &= check(m.background == scintilla_iprgb_color(0xFF0000), id,
                     "marker background color must match the configured marker color");
        ok &= check(line != nullptr, id, "marker rect must map to a visual line");
        ok &= check(nearly_equal(m.rect.top(), expected_top), id,
                     "marker rect top must match the target line");
        ok &= check(nearly_equal(m.rect.bottom(), expected_bottom), id,
                     "marker rect bottom must match the target line");
        ok &= check(m.rect.right() <= frame.text_rect.left() + 2.0, id,
                     "marker rect must stay within the gutter");
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
    const char *id = "multi_caret";
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
    ok &= check(frame.caret_primitives.size() == 3, id,
                 "multi-caret fixture must produce exactly three caret primitives");

    // Check that exactly one caret is marked as main.
    int main_count = 0;
    int caret_line = -1;
    std::vector<QRectF> caret_rects;
    for (const auto &caret : frame.caret_primitives) {
        if (caret.is_main) ++main_count;
        ok &= check(caret.rect.width() > 0, id,
            "each caret rect width must be > 0");
        ok &= check(caret.rect.height() > 0, id,
            "each caret rect height must be > 0");
        const visual_line_frame_t *line = find_matching_visual_line(frame, caret.rect);
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
    ok &= check(main_count == 1, id,
                 "exactly one caret must be marked as main");
    for (size_t i = 0; i < caret_rects.size(); ++i) {
        for (size_t j = i + 1; j < caret_rects.size(); ++j) {
            const bool disjoint =
                caret_rects[i].right() <= caret_rects[j].left() + k_geometry_tolerance ||
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
    const char *id = "whitespace_visible";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.editor.send(SCI_SETVIEWWS, SCWS_VISIBLEALWAYS);
    f.set_text("a b\tc");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.whitespace_marks.empty(), id,
                 "visible whitespace must produce whitespace_marks");

    bool has_dot = false;
    bool has_tab = false;
    for (const auto &ws : frame.whitespace_marks) {
        ok &= check(ws.rect.width() > 0, id, "whitespace mark rect width must be > 0");
        ok &= check(ws.rect.height() > 0, id, "whitespace mark rect height must be > 0");
        ok &= check(ws.color.isValid() && ws.color.alpha() > 0, id,
                     "whitespace mark color must be valid and visible");
        if (ws.kind == whitespace_mark_kind_t::space_dot) has_dot = true;
        if (ws.kind == whitespace_mark_kind_t::tab_arrow) has_tab = true;
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
    const char *id = "eol_annotation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\n");
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STANDARD);
    const char *annot_text = "this is an eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0,
                  reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.eol_annotations.empty(), id,
                 "EOL annotation must produce eol_annotations");
    if (!frame.eol_annotations.empty()) {
        const Eol_annotation_primitive &eol = frame.eol_annotations[0];
        ok &= check(eol.text == QString::fromUtf8(annot_text), id,
                     "EOL annotation text must match");
        ok &= check(eol.rect.width() > 0, id,
                     "EOL annotation rect width must be > 0");
        ok &= check(eol.document_line == 0, id,
                     "EOL annotation must be on document line 0");
        ok &= check(eol.foreground.isValid(), id,
                     "EOL annotation foreground must be valid");
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
    const char *id = "annotation";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three");
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD);
    const char *annot_text = "annotation text here";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0,
                  reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id,
                 "annotation must produce annotations");
    if (!frame.annotations.empty()) {
        const Annotation_primitive &annot = frame.annotations[0];
        ok &= check(annot.text == QString::fromUtf8(annot_text), id,
                     "annotation text must match");
        ok &= check(annot.rect.width() > 0, id,
                     "annotation rect width must be > 0");
        ok &= check(annot.document_line == 0, id,
                     "annotation must be on document line 0");
        ok &= check(annot.foreground.isValid(), id,
                     "annotation foreground must be valid");
    }

    ok &= check_no_overlapping_runs(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 7: indent guide
// ---------------------------------------------------------------------------

static bool test_indent_guide()
{
    const char *id = "indent_guide";
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
    ok &= check(!frame.indent_guides.empty(), id,
                 "indent guides must produce indent_guides when SC_IV_REAL is set");
    for (const auto &guide : frame.indent_guides) {
        ok &= check(guide.top < guide.bottom, id,
                     "indent guide top must be < bottom");
        ok &= check(guide.x > 0, id,
                     "indent guide x must be positive");
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
    const char *id = "style_underline";
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
    ok &= check(!frame.decoration_underlines.empty(), id,
                 "style underline must produce decoration_underlines");
    if (!frame.decoration_underlines.empty()) {
        const Decoration_underline_primitive &ul = frame.decoration_underlines[0];
        ok &= check(ul.rect.width() > 0, id,
                     "underline rect width must be > 0");
        ok &= check(ul.kind == decoration_kind_t::style_underline, id,
                     "underline kind must be style_underline");
        ok &= check(ul.color.isValid(), id,
                     "underline color must be valid");
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
    const char *id = "fold_display_text";
    qDebug("--- %s", id);

    Fixture_editor f;
    // Set fold display text style
    f.editor.send(SCI_SETDEFAULTFOLDDISPLAYTEXT, 0,
        reinterpret_cast<sptr_t>("..."));
    f.editor.send(SCI_FOLDDISPLAYTEXTSETSTYLE, SC_FOLDDISPLAYTEXT_BOXED, 0);

    // Create foldable structure via explicit fold levels (no lexer needed)
    f.set_text("header line\n    body1\n    body2\nclosing");
    f.editor.send(SCI_SETFOLDLEVEL, 0,
        SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
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
    ok &= check(!frame.fold_display_texts.empty(), id,
                 "collapsing a fold must produce fold_display_texts");
    if (!frame.fold_display_texts.empty()) {
        const Fold_display_text_primitive &fold = frame.fold_display_texts[0];
        ok &= check(!fold.text.isEmpty(), id,
                     "fold display text must have non-empty text");
        ok &= check(fold.rect.width() > 0, id,
                     "fold display text rect width must be > 0");
        ok &= check(fold.rect.height() > 0, id,
                     "fold display text rect height must be > 0");
        ok &= check(fold.foreground.isValid(), id,
                     "fold display text must have valid foreground color");
        ok &= check(fold.document_line == 0, id,
                     "fold display text must be on the folded line");
        ok &= check(fold.boxed, id,
                     "fold display text must have boxed=true for SC_FOLDDISPLAYTEXT_BOXED");
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
    const char *id = "marker_fold_part";
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
    f.editor.send(SCI_SETFOLDLEVEL, 0,
        SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
    f.editor.send(SCI_SETFOLDLEVEL, 1, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 2, SC_FOLDLEVELBASE + 1);
    f.editor.send(SCI_SETFOLDLEVEL, 3, SC_FOLDLEVELBASE);

    // Enable fold block highlighting and place caret inside the fold body
    // so HighlightDelimiter populates fold parts.
    f.editor.send(SCI_MARKERENABLEHIGHLIGHT, 1);
    f.editor.send(SCI_GOTOPOS,
        f.editor.send(SCI_POSITIONFROMLINE, 1));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    // Should have fold markers in the margin
    ok &= check(!frame.marker_primitives.empty(), id,
                 "fold margin with fold levels must produce marker_primitives");

    // Verify fold_part is carried through for at least some markers
    bool has_nonzero_fold_part = false;
    bool has_selected_background = false;
    for (const auto &m : frame.marker_primitives) {
        if (m.fold_part > 0) {
            has_nonzero_fold_part = true;
        }
        if (m.background_selected.isValid() && m.background_selected.alpha() > 0) {
            has_selected_background = true;
        }
        // fold_part must be in valid range [0..4]
        ok &= check(m.fold_part >= 0 && m.fold_part <= 4, id,
                     "marker fold_part must be in range [0..4]");
    }
    ok &= check(has_nonzero_fold_part, id,
                 "at least one marker must have non-zero fold_part");
    ok &= check(has_selected_background, id,
                 "highlighted fold markers must preserve selected background color");

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 8: annotation with boxed outline
// ---------------------------------------------------------------------------

static bool test_annotation_boxed()
{
    const char *id = "annotation_boxed";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three");

    const sptr_t annotation_style_offset =
        f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_ANNOTATIONSETSTYLEOFFSET, annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, annotation_style_offset, 0xC2F4FF);

    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED, 0);
    f.editor.send(SCI_ANNOTATIONSETSTYLE, 0, 0);
    const char *annot_text = "boxed annotation text";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 0,
        reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id,
                 "boxed annotation must produce annotation primitives");
    if (!frame.annotations.empty()) {
        const Annotation_primitive &annot = frame.annotations[0];
        ok &= check(!annot.text.isEmpty(), id,
                     "annotation text must be non-empty");
        ok &= check(annot.rect.width() > 0, id,
                     "annotation rect width must be > 0");
        ok &= check(annot.rect.height() > 0, id,
                     "annotation rect height must be > 0");
        ok &= check(annot.document_line == 0, id,
                     "annotation must be on the correct line");
        ok &= check(annot.boxed, id,
                     "annotation must have boxed=true for ANNOTATION_BOXED style");
        ok &= check(annot.foreground.isValid(), id,
                     "annotation must have valid foreground color");
        ok &= check(annot.background.isValid(), id,
                     "annotation must have valid background color");
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
    const char *id = "ltr_direction_field";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("Hello World");

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.visual_lines.empty(), id,
                 "text must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const visual_line_frame_t &line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id,
                     "text must produce text runs");
        for (const text_run_t &run : line.text_runs) {
            ok &= check(run.direction == text_direction_t::left_to_right, id,
                         "Latin text runs must have left_to_right direction");
        }
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

static bool test_rtl_direction_field()
{
    const char *id = "utf8_hebrew_capture";
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
    ok &= check(!frame.visual_lines.empty(), id,
                 "text with Hebrew must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const visual_line_frame_t &line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id,
                     "text with Hebrew must produce text runs");
        for (const text_run_t &run : line.text_runs) {
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
    const char *id = "mixed_utf8_capture";
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
    ok &= check(!frame.visual_lines.empty(), id,
                 "mixed BiDi text must produce visual lines");
    if (!frame.visual_lines.empty()) {
        const visual_line_frame_t &line = frame.visual_lines[0];
        ok &= check(!line.text_runs.empty(), id,
                     "mixed BiDi text must produce text runs");

        for (const text_run_t &run : line.text_runs) {
            ok &= check(run.width > 0 || run.text.isEmpty(), id,
                         "all non-empty runs must have positive width");
        }

        QString full_text = reconstruct_line_text(line);
        ok &= check(full_text.contains("Hello"), id,
                     "mixed text must contain Latin portion");
        ok &= check(full_text.contains("World"), id,
                     "mixed text must contain trailing Latin portion");
    }

    ok &= check_no_overlapping_runs(frame, id);
    ok &= check_runs_positive_width(frame, id);
    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

static bool test_annotation_boxed_padding()
{
    const char *id = "annotation_boxed_padding";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\nline three\n");
    f.editor.send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED);
    f.editor.send(SCI_ANNOTATIONSETSTYLE, 1, STYLE_DEFAULT);
    const char *annot_text = "boxed annotation with padding";
    f.editor.send(SCI_ANNOTATIONSETTEXT, 1,
                  reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.annotations.empty(), id,
                 "boxed annotation must produce annotation primitives");

    for (const Annotation_primitive &annot : frame.annotations) {
        ok &= check(annot.boxed, id,
                     "annotation must have boxed flag set");
        ok &= check(annot.rect.width() > 0, id,
                     "boxed annotation rect width must be > 0");
        ok &= check(annot.rect.height() > 0, id,
                     "boxed annotation rect height must be > 0");
        ok &= check(annot.position.x() >= annot.rect.left(), id,
                     "annotation text X must be >= rect left (padding)");
        ok &= check(annot.position.x() < annot.rect.right(), id,
                     "annotation text X must be < rect right");
        ok &= check(annot.baseline_y >= annot.rect.top(), id,
                     "annotation baseline must be >= rect top");
        ok &= check(annot.baseline_y <= annot.rect.bottom(), id,
                     "annotation baseline must be <= rect bottom");
    }

    ok &= check_visual_lines_no_vertical_overlap(frame, id);
    return ok;
}

// ---------------------------------------------------------------------------
// Phase 9: EOL annotation boxed (visible_style fidelity)
// ---------------------------------------------------------------------------

static bool test_eol_annotation_boxed()
{
    const char *id = "eol_annotation_boxed";
    qDebug("--- %s", id);

    Fixture_editor f;
    f.set_text("line one\nline two\n");
    const sptr_t eol_annotation_style_offset =
        f.editor.send(SCI_ALLOCATEEXTENDEDSTYLES, 1);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLEOFFSET, eol_annotation_style_offset);
    f.editor.send(SCI_STYLESETFORE, eol_annotation_style_offset, 0x000000);
    f.editor.send(SCI_STYLESETBACK, eol_annotation_style_offset, 0xC2F4FF);
    f.editor.send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_BOXED);
    f.editor.send(SCI_EOLANNOTATIONSETSTYLE, 0, 0);
    const char *annot_text = "boxed eol annotation";
    f.editor.send(SCI_EOLANNOTATIONSETTEXT, 0,
                  reinterpret_cast<sptr_t>(annot_text));
    f.pump();

    Render_frame frame = f.capture();
    dump_frame_summary(id, frame);

    bool ok = true;
    ok &= check(!frame.eol_annotations.empty(), id,
                 "boxed EOL annotation must produce eol_annotations");
    if (!frame.eol_annotations.empty()) {
        const Eol_annotation_primitive &eol = frame.eol_annotations[0];
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
    const char *id = "overlapping_indicators";
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
    ok &= check(frame.indicator_primitives.size() >= 2, id,
                 "overlapping indicators must produce multiple primitives");

    bool has_squiggle = false;
    bool has_box = false;
    for (const Indicator_primitive &ind : frame.indicator_primitives) {
        if (ind.indicator_style == INDIC_SQUIGGLE) has_squiggle = true;
        if (ind.indicator_style == INDIC_BOX) has_box = true;
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

static void stderrMessageHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    fprintf(stderr, "%s\n", qPrintable(msg));
    fflush(stderr);
}

int main(int argc, char **argv)
{
    qInstallMessageHandler(stderrMessageHandler);

    // Pin DPI and scale factor for deterministic metrics even when run
    // outside of CTest (which sets these in the environment).
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    QGuiApplication app(argc, argv);

    // Verify the test font is actually available.  Silent substitution
    // would invalidate every width/wrap/overlap assertion.
    {
        QFont probe(QStringLiteral("Consolas"), 11);
        QFontInfo info(probe);
        if (info.family().compare(QStringLiteral("Consolas"),
                                  Qt::CaseInsensitive) != 0) {
            qCritical("FAIL: Consolas font not available (resolved to '%s'). "
                      "Frame validation requires Consolas for deterministic metrics.",
                      qPrintable(info.family()));
            return 1;
        }
    }

    qDebug("=== Frame Validation (phases 2-9, structural + semantic) ===\n");

    struct { const char *name; bool (*fn)(); } fixtures[] = {
        // Phase 2 fixtures
        {"plain_ascii_short",        test_plain_ascii_short},
        {"plain_ascii_long_wrap",    test_plain_ascii_long_wrap},
        {"horizontal_scroll_resets_on_doc_switch", test_horizontal_scroll_resets_on_doc_switch},
        {"caret_left_scrolls_to_long_previous_line", test_caret_left_scrolls_to_long_previous_line},
        {"mixed_styles_wrap",        test_mixed_styles_wrap},
        {"tab_layout_default",       test_tab_layout_default},
        {"tab_layout_nondefault",    test_tab_layout_nondefault},
        {"selection_single_line",    test_selection_single_line},
        {"selection_wrap_boundary",  test_selection_wrap_boundary},
        {"caret_mid_line",           test_caret_mid_line},
        {"caret_wrap_continuation",  test_caret_wrap_continuation},
        {"margin_numbers_basic",     test_margin_numbers_basic},
        {"margin_numbers_wrap",      test_margin_numbers_wrap},
        {"plain_indicator_basic",    test_plain_indicator_basic},
        // Phase 3: multi-selection, rectangular selection, current-line
        {"multi_selection",          test_multi_selection},
        {"rectangular_selection",    test_rectangular_selection},
        {"current_line_frame",       test_current_line_frame},
        // Phase 4: expanded indicator styles
        {"squiggle_indicator",       test_squiggle_indicator},
        {"box_indicator",            test_box_indicator},
        // Phase 5: marker symbol
        {"marker_symbol",            test_marker_symbol},
        // Phase 6: multi-caret
        {"multi_caret",              test_multi_caret},
        // Phase 7: new visual families
        {"whitespace_visible",       test_whitespace_visible},
        {"eol_annotation",           test_eol_annotation},
        {"annotation",               test_annotation},
        {"indent_guide",             test_indent_guide},
        {"style_underline",          test_style_underline},
        // Phase 8: fold/marker/annotation fidelity
        {"fold_display_text",        test_fold_display_text},
        {"marker_fold_part",         test_marker_fold_part},
        {"annotation_boxed",         test_annotation_boxed},
        // Phase 9: UTF-8 text-run capture, boxed EOL annotations, overlapping indicators
        {"ltr_direction_field",      test_ltr_direction_field},
        {"utf8_hebrew_capture",      test_rtl_direction_field},
        {"mixed_utf8_capture",       test_mixed_bidi_direction},
        {"annotation_boxed_padding", test_annotation_boxed_padding},
        {"eol_annotation_boxed",     test_eol_annotation_boxed},
        {"overlapping_indicators",   test_overlapping_indicators},
    };

    int fixture_pass = 0;
    int fixture_fail = 0;

    for (const auto &f : fixtures) {
        bool ok = f.fn();
        if (ok) {
            qDebug("  PASS [%s]\n", f.name);
            ++fixture_pass;
        } else {
            qWarning("  FIXTURE FAILED [%s]\n", f.name);
            ++fixture_fail;
        }
    }

    qDebug("=== Results: %d/%d fixtures passed, %d/%d individual checks passed ===",
           fixture_pass, fixture_pass + fixture_fail,
           g_pass_count, g_pass_count + g_fail_count);

    if (fixture_fail > 0) {
        qDebug("NOTE: Failures indicate the frame capture pipeline does not yet "
               "produce expected primitives for these corpus cases.");
    }

    return fixture_fail > 0 ? 1 : 0;
}
