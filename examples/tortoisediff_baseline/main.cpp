// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include "scintillaquick_font.h"
#include "Scintilla.h"

namespace
{

class CallbackEventFilter : public QObject
{
public:
    explicit CallbackEventFilter(std::function<void()> callback) : callback_(std::move(callback)) {}

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            callback_();
        }
        return false;
    }

private:
    std::function<void()> callback_;
};

constexpr int rgb(int red, int green, int blue)
{
    return red | (green << 8) | (blue << 16);
}

constexpr int k_editor_foreground = rgb(31, 35, 40);
constexpr int k_editor_background = rgb(255, 255, 255);
constexpr int k_margin_foreground = rgb(87, 96, 106);
constexpr int k_margin_background = rgb(246, 248, 250);
constexpr int k_selection_background = rgb(72, 118, 205);
constexpr int k_marker_added = 0;
constexpr int k_marker_deleted = 1;
constexpr int k_marker_changed = 2;
constexpr int k_marker_filler = 3;
constexpr int k_inline_changed_indicator = 0;
constexpr int k_added_background = rgb(205, 240, 209);
constexpr int k_deleted_background = rgb(251, 207, 207);
constexpr int k_changed_background = rgb(252, 239, 197);
constexpr int k_filler_background = rgb(212, 219, 229);
constexpr int k_inline_changed_background = rgb(245, 192, 80);

enum class DiffSideState
{
    Equal,
    Added,
    Deleted,
    Changed,
    Filler
};

struct DiffRow
{
    int hunkId;
    int changedGroupId;
    int leftSourceLine;
    int rightSourceLine;
    DiffSideState leftState;
    DiffSideState rightState;
};

struct DiffWidgetInput
{
    QString leftText;
    QString rightText;
    std::vector<DiffRow> rows;
};

struct InlineChangedSpan
{
    int begin;
    int end;
};

QStringList source_lines_from_text(const QString& text)
{
    return text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
}

std::pair<InlineChangedSpan, InlineChangedSpan> inline_changed_spans(const QString& left, const QString& right)
{
    const int shared_length = std::min(left.size(), right.size());
    int prefix = 0;
    while (prefix < shared_length && left[prefix] == right[prefix]) {
        ++prefix;
    }

    int left_end = left.size();
    int right_end = right.size();
    while (left_end > prefix && right_end > prefix && left[left_end - 1] == right[right_end - 1]) {
        --left_end;
        --right_end;
    }

    return {InlineChangedSpan{prefix, left_end}, InlineChangedSpan{prefix, right_end}};
}

int utf8_size(const QString& text, int length)
{
    return text.left(length).toUtf8().size();
}

void append_change_block(std::vector<DiffRow>& rows, int& next_hunk_id, int& next_group_id, int left_begin,
    int left_end, int right_begin, int right_end)
{
    const int left_count = left_end - left_begin;
    const int right_count = right_end - right_begin;
    if (left_count == 0 && right_count == 0) {
        return;
    }

    const int hunk_id = next_hunk_id++;
    const int group_id = next_group_id++;
    const int paired_count = std::min(left_count, right_count);
    for (int offset = 0; offset < paired_count; ++offset) {
        rows.push_back({hunk_id, group_id, left_begin + offset + 1, right_begin + offset + 1,
            DiffSideState::Changed, DiffSideState::Changed});
    }
    for (int offset = paired_count; offset < left_count; ++offset) {
        rows.push_back({hunk_id, group_id, left_begin + offset + 1, -1, DiffSideState::Deleted,
            DiffSideState::Filler});
    }
    for (int offset = paired_count; offset < right_count; ++offset) {
        rows.push_back({hunk_id, group_id, -1, right_begin + offset + 1, DiffSideState::Filler,
            DiffSideState::Added});
    }
}

struct DiffHunkHeader
{
    int leftStart;
    int leftCount;
    int rightStart;
    int rightCount;
};

bool parse_hunk_range(const QString& range, int& start, int& count)
{
    const int comma_index = range.indexOf(QLatin1Char(','));
    bool ok = false;
    if (comma_index == -1) {
        start = range.toInt(&ok);
        count = 1;
        return ok && start >= 0;
    }

    start = range.left(comma_index).toInt(&ok);
    if (!ok || start < 0) {
        return false;
    }

    count = range.mid(comma_index + 1).toInt(&ok);
    return ok && count >= 0;
}

bool parse_hunk_header(const QString& line, DiffHunkHeader& header)
{
    if (!line.startsWith(QStringLiteral("@@ -"))) {
        return false;
    }

    const int right_marker = line.indexOf(QStringLiteral(" +"), 4);
    if (right_marker == -1) {
        return false;
    }

    const int close_marker = line.indexOf(QStringLiteral(" @@"), right_marker + 2);
    if (close_marker == -1) {
        return false;
    }

    const QString left_range = line.mid(4, right_marker - 4);
    const QString right_range = line.mid(right_marker + 2, close_marker - right_marker - 2);
    return parse_hunk_range(left_range, header.leftStart, header.leftCount) &&
        parse_hunk_range(right_range, header.rightStart, header.rightCount);
}

DiffWidgetInput diff_input_from_unified_diff(const QString& unified_diff)
{
    const QStringList diff_lines = unified_diff.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QStringList left_lines;
    QStringList right_lines;
    std::vector<DiffRow> rows;
    rows.reserve(static_cast<size_t>(diff_lines.size()));

    int next_hunk_id = 1;
    int next_group_id = 1;
    int left_block_begin = 0;
    int right_block_begin = 0;
    int hunk_left_seen = 0;
    int hunk_right_seen = 0;
    bool in_change_block = false;
    bool in_hunk = false;
    bool saw_hunk = false;
    bool saw_file_header = false;
    DiffHunkHeader active_hunk = {};

    const auto flush_change_block = [&]() {
        if (!in_change_block) {
            return;
        }

        append_change_block(rows, next_hunk_id, next_group_id, left_block_begin,
            static_cast<int>(left_lines.size()), right_block_begin, static_cast<int>(right_lines.size()));
        in_change_block = false;
    };

    const auto finish_hunk = [&]() {
        if (!in_hunk) {
            return;
        }

        flush_change_block();
        if (hunk_left_seen != active_hunk.leftCount || hunk_right_seen != active_hunk.rightCount) {
            qFatal("TortoiseDiff Step 9 fixture hunk has %d left/%d right lines, expected %d left/%d right.",
                hunk_left_seen, hunk_right_seen, active_hunk.leftCount, active_hunk.rightCount);
        }
        in_hunk = false;
    };

    const auto start_change_block = [&]() {
        if (in_change_block) {
            return;
        }

        left_block_begin = static_cast<int>(left_lines.size());
        right_block_begin = static_cast<int>(right_lines.size());
        in_change_block = true;
    };

    for (int line_index = 0; line_index < diff_lines.size(); ++line_index) {
        QString diff_line = diff_lines[line_index];
        if (diff_line.endsWith(QLatin1Char('\r'))) {
            diff_line.chop(1);
        }
        if (line_index == diff_lines.size() - 1 && diff_line.isEmpty()) {
            break;
        }

        DiffHunkHeader parsed_hunk = {};
        if (parse_hunk_header(diff_line, parsed_hunk)) {
            finish_hunk();
            active_hunk = parsed_hunk;
            hunk_left_seen = 0;
            hunk_right_seen = 0;
            in_hunk = true;
            saw_hunk = true;
            continue;
        }

        if (!in_hunk) {
            if (diff_line.isEmpty()) {
                continue;
            }
            if (diff_line.startsWith(QStringLiteral("diff --git "))) {
                if (saw_file_header || saw_hunk) {
                    qFatal("TortoiseDiff Step 9 fixture supports one unified-diff file only.");
                }
                saw_file_header = true;
                continue;
            }
            if (diff_line.startsWith(QStringLiteral("index ")) || diff_line.startsWith(QStringLiteral("--- ")) ||
                diff_line.startsWith(QStringLiteral("+++ ")))
            {
                continue;
            }

            qFatal("TortoiseDiff Step 9 fixture has an unexpected line outside a hunk: %s",
                qPrintable(diff_line));
        }

        if (diff_line == QStringLiteral("\\ No newline at end of file")) {
            continue;
        }
        if (diff_line.isEmpty()) {
            qFatal("TortoiseDiff Step 9 fixture has a hunk body line without a diff prefix.");
        }

        const QChar prefix = diff_line.at(0);
        const QString text = diff_line.mid(1);
        if (prefix == QLatin1Char(' ')) {
            flush_change_block();
            left_lines.append(text);
            right_lines.append(text);
            rows.push_back({0, -1, static_cast<int>(left_lines.size()), static_cast<int>(right_lines.size()),
                DiffSideState::Equal, DiffSideState::Equal});
            ++hunk_left_seen;
            ++hunk_right_seen;
        } else if (prefix == QLatin1Char('-')) {
            start_change_block();
            left_lines.append(text);
            ++hunk_left_seen;
        } else if (prefix == QLatin1Char('+')) {
            start_change_block();
            right_lines.append(text);
            ++hunk_right_seen;
        } else {
            qFatal("TortoiseDiff Step 9 fixture has an unsupported hunk body prefix: %s",
                qPrintable(diff_line.left(1)));
        }
    }

    finish_hunk();
    if (!saw_hunk) {
        qFatal("TortoiseDiff Step 9 fixture must contain at least one hunk.");
    }

    return {left_lines.join(QStringLiteral("\n")), right_lines.join(QStringLiteral("\n")), std::move(rows)};
}

bool source_and_state_match(int source_line, DiffSideState state)
{
    return (source_line == -1) == (state == DiffSideState::Filler);
}

QString render_side_text(const DiffWidgetInput& input, bool left_side)
{
    const QStringList lines = source_lines_from_text(left_side ? input.leftText : input.rightText);
    QString text;
    for (int row_index = 0; row_index < static_cast<int>(input.rows.size()); ++row_index) {
        if (row_index > 0) {
            text += QLatin1Char('\n');
        }

        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        const int source_line = left_side ? row.leftSourceLine : row.rightSourceLine;
        if (source_line != -1) {
            text += lines[source_line - 1];
        }
    }

    return text;
}

QString source_side_copy_text(
    const QString& display_copy_text, const std::vector<DiffRow>& rows, bool left_side, int first_display_row)
{
    const QStringList copied_lines = display_copy_text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QStringList source_lines;
    for (int index = 0; index < copied_lines.size(); ++index) {
        const int row_index = first_display_row + index;
        if (row_index < 0 || row_index >= static_cast<int>(rows.size())) {
            continue;
        }

        const DiffRow& row = rows[static_cast<size_t>(row_index)];
        const int source_line = left_side ? row.leftSourceLine : row.rightSourceLine;
        if (source_line != -1) {
            source_lines.append(copied_lines[index]);
        }
    }
    return source_lines.join(QLatin1Char('\n'));
}

std::vector<DiffRow> raw_text_diff_rows(const QString& left_text, const QString& right_text)
{
    const QStringList left_lines = source_lines_from_text(left_text);
    const QStringList right_lines = source_lines_from_text(right_text);
    const int left_count = static_cast<int>(left_lines.size());
    const int right_count = static_cast<int>(right_lines.size());

    std::vector<std::vector<int>> lcs(
        static_cast<size_t>(left_count + 1), std::vector<int>(static_cast<size_t>(right_count + 1), 0));
    for (int left = left_count - 1; left >= 0; --left) {
        for (int right = right_count - 1; right >= 0; --right) {
            if (left_lines[left] == right_lines[right]) {
                lcs[static_cast<size_t>(left)][static_cast<size_t>(right)] =
                    lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right + 1)] + 1;
            } else {
                lcs[static_cast<size_t>(left)][static_cast<size_t>(right)] =
                    std::max(lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right)],
                        lcs[static_cast<size_t>(left)][static_cast<size_t>(right + 1)]);
            }
        }
    }

    std::vector<DiffRow> rows;
    rows.reserve(static_cast<size_t>(left_count + right_count));
    int next_hunk_id = 1;

    const auto append_block = [&](int left_begin, int left_end, int right_begin, int right_end) {
        if (left_begin == left_end && right_begin == right_end) {
            return;
        }

        const int hunk_id = next_hunk_id;
        const int changed_group_id = next_hunk_id;
        const int row_count = std::max(left_end - left_begin, right_end - right_begin);
        for (int row = 0; row < row_count; ++row) {
            const bool has_left = left_begin + row < left_end;
            const bool has_right = right_begin + row < right_end;
            rows.push_back({
                hunk_id,
                changed_group_id,
                has_left ? left_begin + row + 1 : -1,
                has_right ? right_begin + row + 1 : -1,
                has_left ? (has_right ? DiffSideState::Changed : DiffSideState::Deleted) : DiffSideState::Filler,
                has_right ? (has_left ? DiffSideState::Changed : DiffSideState::Added) : DiffSideState::Filler,
            });
        }
        ++next_hunk_id;
    };

    int left = 0;
    int right = 0;
    while (left < left_count && right < right_count) {
        if (left_lines[left] == right_lines[right]) {
            rows.push_back({0, -1, left + 1, right + 1, DiffSideState::Equal, DiffSideState::Equal});
            ++left;
            ++right;
            continue;
        }

        const int left_begin = left;
        const int right_begin = right;
        while (left < left_count && right < right_count && left_lines[left] != right_lines[right]) {
            if (lcs[static_cast<size_t>(left)][static_cast<size_t>(right + 1)] >=
                lcs[static_cast<size_t>(left + 1)][static_cast<size_t>(right)])
            {
                ++right;
            } else {
                ++left;
            }
        }
        if (left == left_count || right == right_count) {
            left = left_count;
            right = right_count;
        }
        append_block(left_begin, left, right_begin, right);
    }
    append_block(left, left_count, right, right_count);

    return rows;
}

int display_row_count(const QString& text)
{
    return text.count(QLatin1Char('\n')) + 1;
}

void validate_source_line_references(
    const std::vector<DiffRow>& rows, bool left_side, int source_line_count, const char* side)
{
    int expected_source_line = 1;
    for (const DiffRow& row : rows) {
        const int source_line = left_side ? row.leftSourceLine : row.rightSourceLine;
        if (source_line == -1) {
            continue;
        }

        if (source_line != expected_source_line || source_line > source_line_count) {
            qFatal("TortoiseDiff Step 9 widget input has invalid %s source line %d; expected %d.", side, source_line,
                expected_source_line);
        }
        ++expected_source_line;
    }

    if (expected_source_line - 1 != source_line_count) {
        qFatal("TortoiseDiff Step 9 widget input has %d %s source lines but references %d.", source_line_count, side,
            expected_source_line - 1);
    }
}

void validate_diff_input(const DiffWidgetInput& input)
{
    if (input.rows.empty()) {
        qFatal("TortoiseDiff Step 9 widget input must not be empty.");
    }

    validate_source_line_references(input.rows, true, display_row_count(input.leftText), "left");
    validate_source_line_references(input.rows, false, display_row_count(input.rightText), "right");

    for (const DiffRow& row : input.rows) {
        if (!source_and_state_match(row.leftSourceLine, row.leftState) ||
            !source_and_state_match(row.rightSourceLine, row.rightState))
        {
            qFatal("TortoiseDiff Step 9 widget input has filler state/source-line mismatch.");
        }
    }

    const QString left_display_text = render_side_text(input, true);
    const QString right_display_text = render_side_text(input, false);
    const int expected_rows = static_cast<int>(input.rows.size());
    if (display_row_count(left_display_text) != expected_rows || display_row_count(right_display_text) != expected_rows) {
        qFatal("TortoiseDiff Step 9 widget input display buffers must have the same display row count as the row model.");
    }
}

QString sample_unified_diff_fixture()
{
    QStringList lines = {
        QStringLiteral("diff --git a/src/widget.cpp b/src/widget.cpp"),
        QStringLiteral("index 13579ab..24680cd 100644"),
        QStringLiteral("--- a/src/widget.cpp"),
        QStringLiteral("+++ b/src/widget.cpp"),
        QStringLiteral("@@ -1,109 +1,110 @@"),
        QStringLiteral(" src/widget.cpp"),
        QStringLiteral(" "),
        QStringLiteral(" int calculate_total(int value)"),
        QStringLiteral(" {"),
        QStringLiteral("-\tconst int adjustment = 1;"),
        QStringLiteral("+\tconst int adjustment = 2;"),
        QStringLiteral(" \treturn value + adjustment;"),
        QStringLiteral(" }"),
        QStringLiteral(" "),
        QStringLiteral(" void configure_options(Config& config)"),
        QStringLiteral(" {"),
        QStringLiteral(" \tconfig.enableCache();"),
        QStringLiteral("-\tconfig.enableDiagnostics();"),
        QStringLiteral("+\tconfig.enableMetrics();"),
        QStringLiteral("+\tconfig.enableTracing();"),
        QStringLiteral(" }"),
        QStringLiteral(" "),
        QStringLiteral(" void remove_legacy_path()"),
        QStringLiteral(" {"),
        QStringLiteral("-\tcleanupLegacyPath();"),
        QStringLiteral("-\tcleanupTempDir();"),
        QStringLiteral("+\tcleanupDeprecatedPaths();"),
        QStringLiteral(" \twriteAuditLine();"),
        QStringLiteral(" }"),
        QStringLiteral(" "),
        QStringLiteral(" QString format_label(QStringView key, QStringView value)"),
        QStringLiteral(" {"),
        QStringLiteral("-\tconst QString separator = QStringLiteral(\"=\");"),
        QStringLiteral("-\treturn key.toString() + separator + value.toString();"),
        QStringLiteral("+\tconst QString label = key.toString();"),
        QStringLiteral("+\tconst QString separator = QStringLiteral(\": \");"),
        QStringLiteral("+\treturn label + separator + value.toString();"),
        QStringLiteral(" }"),
        QStringLiteral(" "),
        QStringLiteral(" // This deliberately long line stays unwrapped in Step 9 so horizontal scrolling can be "
                       "checked while left/right display rows are aligned with blank filler buffer lines.")};

    for (int row = 1; row <= 80; ++row) {
        lines.append(QStringLiteral(" unchanged display row %1 keeps vertical synchronization checkable.")
                         .arg(row, 2, 10, QLatin1Char('0')));
    }

    lines.append(QStringLiteral(" // End of synchronized scrolling fixture."));
    return lines.join(QStringLiteral("\n"));
}

QString live_unified_diff_or_fixture()
{
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    git.setWorkingDirectory(QDir::currentPath());
    // ponytail: one hardcoded full-context file keeps this demo deterministic until real file selection exists.
    git.setArguments({QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--unified=100000"),
        QStringLiteral("--"), QStringLiteral("examples/tortoisediff_baseline/main.cpp")});

    git.start();
    if (!git.waitForStarted(1000)) {
        return sample_unified_diff_fixture();
    }
    if (!git.waitForFinished(3000)) {
        git.kill();
        git.waitForFinished();
        return sample_unified_diff_fixture();
    }
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        return sample_unified_diff_fixture();
    }

    const QString stdout_diff = QString::fromUtf8(git.readAllStandardOutput());
    return stdout_diff.isEmpty() ? sample_unified_diff_fixture() : stdout_diff;
}

DiffWidgetInput sample_diff_input()
{
    DiffWidgetInput input = diff_input_from_unified_diff(live_unified_diff_or_fixture());
    validate_diff_input(input);
    return input;
}

void configure_pane(ScintillaQuick_item& pane, const QFont& font, const QString& text)
{
    pane.setProperty("font", font);
    pane.send(SCI_SETTABWIDTH, 4);
    pane.send(SCI_SETWRAPMODE, SC_WRAP_NONE);

    pane.send(SCI_STYLESETFORE, STYLE_DEFAULT, k_editor_foreground);
    pane.send(SCI_STYLESETBACK, STYLE_DEFAULT, k_editor_background);
    pane.send(SCI_STYLECLEARALL);

    pane.send(SCI_STYLESETFORE, STYLE_LINENUMBER, k_margin_foreground);
    pane.send(SCI_STYLESETBACK, STYLE_LINENUMBER, k_margin_background);

    // Stock Scintilla number margins show display-buffer line numbers. Source
    // gutters are intentionally left for later steps.
    const int margin_count = static_cast<int>(pane.send(SCI_GETMARGINS));
    for (int margin = 1; margin < margin_count; ++margin) {
        pane.send(SCI_SETMARGINWIDTHN, margin, 0);
    }
    pane.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    const int line_number_width =
        static_cast<int>(pane.send(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>("9999")));
    pane.send(SCI_SETMARGINWIDTHN, 0, line_number_width + 12);
    pane.send(SCI_SETMARGINLEFT, 2);
    pane.send(SCI_SETMARGINRIGHT, 4);

    pane.send(SCI_SETSELFORE, 1, k_editor_foreground);
    pane.send(SCI_SETSELBACK, 1, k_selection_background);
    pane.send(SCI_SETSELALPHA, 80);
    pane.send(SCI_SETCARETFORE, k_editor_background);
    pane.send(SCI_SETCARETWIDTH, 0);

    pane.setProperty("text", text);
    pane.setProperty("readonly", true);
}

void configure_diff_marker(ScintillaQuick_item& pane, int marker, int background)
{
    pane.send(SCI_MARKERDEFINE, marker, SC_MARK_FULLRECT);
    pane.send(SCI_MARKERSETBACK, marker, background);
    pane.send(SCI_MARKERSETLAYER, marker, SC_LAYER_UNDER_TEXT);
}

void configure_diff_markers(ScintillaQuick_item& pane)
{
    const int margin_count = static_cast<int>(pane.send(SCI_GETMARGINS));
    for (int margin = 0; margin < margin_count; ++margin) {
        pane.send(SCI_SETMARGINMASKN, margin, 0);
    }

    configure_diff_marker(pane, k_marker_added, k_added_background);
    configure_diff_marker(pane, k_marker_deleted, k_deleted_background);
    configure_diff_marker(pane, k_marker_changed, k_changed_background);
    configure_diff_marker(pane, k_marker_filler, k_filler_background);
}

int marker_for_state(DiffSideState state)
{
    switch (state) {
        case DiffSideState::Added:
            return k_marker_added;
        case DiffSideState::Deleted:
            return k_marker_deleted;
        case DiffSideState::Changed:
            return k_marker_changed;
        case DiffSideState::Filler:
            return k_marker_filler;
        case DiffSideState::Equal:
            return -1;
    }

    return -1;
}

void apply_diff_markers(ScintillaQuick_item& pane, const DiffWidgetInput& input, bool left_side)
{
    configure_diff_markers(pane);
    for (int marker : std::array<int, 4>{k_marker_added, k_marker_deleted, k_marker_changed, k_marker_filler}) {
        pane.send(SCI_MARKERDELETEALL, marker);
    }
    for (int row_index = 0; row_index < static_cast<int>(input.rows.size()); ++row_index) {
        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        const DiffSideState state = left_side ? row.leftState : row.rightState;
        const int marker = marker_for_state(state);
        if (marker != -1) {
            pane.send(SCI_MARKERADD, row_index, marker);
        }
    }
}

void configure_inline_changed_text_highlights(ScintillaQuick_item& pane)
{
    pane.send(SCI_INDICSETSTYLE, k_inline_changed_indicator, INDIC_FULLBOX);
    pane.send(SCI_INDICSETFORE, k_inline_changed_indicator, k_inline_changed_background);
    pane.send(SCI_INDICSETUNDER, k_inline_changed_indicator, 1);
    pane.send(SCI_INDICSETALPHA, k_inline_changed_indicator, 90);
    pane.send(SCI_INDICSETOUTLINEALPHA, k_inline_changed_indicator, 120);
}

void apply_inline_changed_text_highlight(
    ScintillaQuick_item& pane, int row_index, const QString& line, const InlineChangedSpan& span)
{
    if (span.end <= span.begin) {
        return;
    }

    const int start_position =
        static_cast<int>(pane.send(SCI_POSITIONFROMLINE, row_index)) + utf8_size(line, span.begin);
    const int length = utf8_size(line.mid(span.begin, span.end - span.begin), span.end - span.begin);
    pane.send(SCI_SETINDICATORCURRENT, k_inline_changed_indicator);
    pane.send(SCI_INDICATORFILLRANGE, start_position, length);
}

void apply_inline_changed_text_highlights(
    ScintillaQuick_item& left, ScintillaQuick_item& right, const DiffWidgetInput& input)
{
    configure_inline_changed_text_highlights(left);
    configure_inline_changed_text_highlights(right);
    left.send(SCI_SETINDICATORCURRENT, k_inline_changed_indicator);
    left.send(SCI_INDICATORCLEARRANGE, 0, left.send(SCI_GETLENGTH));
    right.send(SCI_SETINDICATORCURRENT, k_inline_changed_indicator);
    right.send(SCI_INDICATORCLEARRANGE, 0, right.send(SCI_GETLENGTH));

    const QStringList left_lines = source_lines_from_text(input.leftText);
    const QStringList right_lines = source_lines_from_text(input.rightText);
    for (int row_index = 0; row_index < static_cast<int>(input.rows.size()); ++row_index) {
        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        if (row.leftState != DiffSideState::Changed || row.rightState != DiffSideState::Changed ||
            row.leftSourceLine == -1 || row.rightSourceLine == -1)
        {
            continue;
        }

        const QString& left_line = left_lines[row.leftSourceLine - 1];
        const QString& right_line = right_lines[row.rightSourceLine - 1];
        const auto [left_span, right_span] = inline_changed_spans(left_line, right_line);
        apply_inline_changed_text_highlight(left, row_index, left_line, left_span);
        apply_inline_changed_text_highlight(right, row_index, right_line, right_span);
    }
}

std::vector<int> hunk_start_display_rows(const DiffWidgetInput& input)
{
    std::vector<int> hunk_rows;
    std::vector<int> seen_hunk_ids;
    for (int row_index = 0; row_index < static_cast<int>(input.rows.size()); ++row_index) {
        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        if (row.hunkId <= 0 ||
            std::find(seen_hunk_ids.begin(), seen_hunk_ids.end(), row.hunkId) != seen_hunk_ids.end())
        {
            continue;
        }

        seen_hunk_ids.push_back(row.hunkId);
        hunk_rows.push_back(row_index);
    }
    return hunk_rows;
}

int hunk_index_for_display_row(const DiffWidgetInput& input, const std::vector<int>& hunk_start_rows, int display_row)
{
    if (display_row < 0 || display_row >= static_cast<int>(input.rows.size())) {
        return -1;
    }

    const int hunk_id = input.rows[static_cast<size_t>(display_row)].hunkId;
    if (hunk_id <= 0) {
        return -1;
    }

    for (int hunk_index = 0; hunk_index < static_cast<int>(hunk_start_rows.size()); ++hunk_index) {
        const int hunk_start_row = hunk_start_rows[static_cast<size_t>(hunk_index)];
        if (hunk_start_row >= 0 && hunk_start_row < static_cast<int>(input.rows.size()) &&
            input.rows[static_cast<size_t>(hunk_start_row)].hunkId == hunk_id)
        {
            return hunk_index;
        }
    }

    return -1;
}

int first_hunk_index_intersecting_display_row_range(
    const DiffWidgetInput& input, const std::vector<int>& hunk_start_rows, int first_display_row, int end_display_row)
{
    if (input.rows.empty()) {
        return -1;
    }

    const int row_count = static_cast<int>(input.rows.size());
    const int first_row = std::clamp(std::min(first_display_row, end_display_row), 0, row_count);
    const int end_row = std::clamp(std::max(first_display_row, end_display_row), 0, row_count);
    for (int row = first_row; row < end_row; ++row) {
        const int hunk_index = hunk_index_for_display_row(input, hunk_start_rows, row);
        if (hunk_index >= 0) {
            return hunk_index;
        }
    }

    return -1;
}

std::pair<int, int> exact_display_row_range(int first_display_row, int end_display_row, int row_count)
{
    if (row_count <= 0) {
        return {-1, -1};
    }

    int first_row = std::clamp(std::min(first_display_row, end_display_row), 0, row_count);
    int end_row = std::clamp(std::max(first_display_row, end_display_row), 0, row_count);
    if (first_row == end_row && first_row < row_count) {
        ++end_row;
    }
    return first_row < end_row ? std::pair{first_row, end_row} : std::pair{-1, -1};
}

std::pair<int, int> hunk_display_row_range(const DiffWidgetInput& input, int hunk_start_row)
{
    if (hunk_start_row < 0 || hunk_start_row >= static_cast<int>(input.rows.size())) {
        return {-1, -1};
    }

    const int hunk_id = input.rows[static_cast<size_t>(hunk_start_row)].hunkId;
    if (hunk_id <= 0) {
        return {-1, -1};
    }

    int first_row = hunk_start_row;
    while (first_row > 0 && input.rows[static_cast<size_t>(first_row - 1)].hunkId == hunk_id) {
        --first_row;
    }

    int end_row = hunk_start_row + 1;
    while (end_row < static_cast<int>(input.rows.size()) &&
           input.rows[static_cast<size_t>(end_row)].hunkId == hunk_id)
    {
        ++end_row;
    }
    return {first_row, end_row};
}

std::pair<int, int> changed_block_range_for_display_row(
    const DiffWidgetInput& input, const std::vector<int>& hunk_start_rows, int display_row)
{
    const int hunk_index = hunk_index_for_display_row(input, hunk_start_rows, display_row);
    return hunk_index >= 0 ?
        hunk_display_row_range(input, hunk_start_rows[static_cast<size_t>(hunk_index)]) :
        exact_display_row_range(display_row, display_row + 1, static_cast<int>(input.rows.size()));
}

int source_line_for_side(const DiffRow& row, bool left_side)
{
    return left_side ? row.leftSourceLine : row.rightSourceLine;
}

const QString& source_text_for_side(const DiffWidgetInput& input, bool left_side)
{
    return left_side ? input.leftText : input.rightText;
}

QString source_text_with_display_row_range_applied(
    const DiffWidgetInput& input, int first_display_row, int end_display_row, bool target_left_side)
{
    const bool source_left_side = !target_left_side;
    const auto [first_row, end_row] =
        exact_display_row_range(first_display_row, end_display_row, static_cast<int>(input.rows.size()));
    if (first_row < 0 || end_row <= first_row) {
        return source_text_for_side(input, target_left_side);
    }

    const QStringList source_lines = source_lines_from_text(source_text_for_side(input, source_left_side));
    const QStringList target_lines = source_lines_from_text(source_text_for_side(input, target_left_side));
    QStringList replacement_lines;
    int target_start = target_lines.size();
    int target_end = target_lines.size();
    bool has_target_lines = false;

    for (int row_index = first_row; row_index < end_row; ++row_index) {
        const DiffRow& row = input.rows[static_cast<size_t>(row_index)];
        const int source_line = source_line_for_side(row, source_left_side);
        if (source_line != -1) {
            replacement_lines.append(source_lines[source_line - 1]);
        }

        const int target_line = source_line_for_side(row, target_left_side);
        if (target_line != -1) {
            if (!has_target_lines) {
                target_start = target_line - 1;
            }
            target_end = target_line;
            has_target_lines = true;
        }
    }

    if (!has_target_lines) {
        for (int row_index = end_row; row_index < static_cast<int>(input.rows.size()); ++row_index) {
            const int target_line = source_line_for_side(input.rows[static_cast<size_t>(row_index)], target_left_side);
            if (target_line != -1) {
                target_start = target_line - 1;
                break;
            }
        }
        target_end = target_start;
    }

    QStringList merged_lines;
    for (int line = 0; line < target_start; ++line) {
        merged_lines.append(target_lines[line]);
    }
    merged_lines.append(replacement_lines);
    for (int line = target_end; line < target_lines.size(); ++line) {
        merged_lines.append(target_lines[line]);
    }

    return merged_lines.join(QLatin1Char('\n'));
}

bool apply_display_row_range_to_target(
    DiffWidgetInput& input, int first_display_row, int end_display_row, bool target_left_side)
{
    const auto [first_row, end_row] =
        exact_display_row_range(first_display_row, end_display_row, static_cast<int>(input.rows.size()));
    if (first_row < 0 || end_row <= first_row) {
        return false;
    }

    const QString merged_text =
        source_text_with_display_row_range_applied(input, first_row, end_row, target_left_side);
    if (target_left_side) {
        input.leftText = merged_text;
    } else {
        input.rightText = merged_text;
    }
    input.rows = raw_text_diff_rows(input.leftText, input.rightText);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qFatal("%s", qPrintable(font_error));
    }

    QQuickWindow window;
    window.setTitle(QStringLiteral("ScintillaQuick TortoiseDiff Step 14 - Merge Actions"));
    window.resize(1200, 720);
    window.setColor(QColor(214, 219, 225));

    auto* root = window.contentItem();

    QQmlEngine qml_engine;
    QQmlComponent scrollbar_component(&qml_engine);
    scrollbar_component.setData(R"qml(
import QtQuick
import QtQuick.Controls

ScrollBar {
    policy: ScrollBar.AlwaysOn
    active: true
    visible: false
    z: 10
}
)qml",
        QUrl());
    const auto create_scrollbar = [&](Qt::Orientation orientation) {
        std::unique_ptr<QObject> scrollbar_object(scrollbar_component.create());
        if (!scrollbar_object) {
            qFatal("TortoiseDiff Step 10 ScrollBar failed to load: %s", qPrintable(scrollbar_component.errorString()));
        }

        auto* scrollbar = qobject_cast<QQuickItem*>(scrollbar_object.get());
        if (!scrollbar) {
            qFatal("TortoiseDiff Step 10 ScrollBar is not a QQuickItem.");
        }
        if (!scrollbar->setProperty("orientation", static_cast<int>(orientation))) {
            qFatal("TortoiseDiff Step 10 ScrollBar orientation property was not writable.");
        }
        scrollbar->setParentItem(root);
        return scrollbar_object;
    };
    std::unique_ptr<QObject> vertical_scrollbar_object = create_scrollbar(Qt::Vertical);
    std::unique_ptr<QObject> horizontal_scrollbar_object = create_scrollbar(Qt::Horizontal);
    auto* vertical_scrollbar = qobject_cast<QQuickItem*>(vertical_scrollbar_object.get());
    auto* horizontal_scrollbar = qobject_cast<QQuickItem*>(horizontal_scrollbar_object.get());

    QQmlComponent hunk_toolbar_component(&qml_engine);
    hunk_toolbar_component.setData(R"qml(
import QtQuick
import QtQuick.Controls

Rectangle {
    color: "#f6f8fa"
    border.color: "#d0d7de"
    z: 20

    Row {
        anchors.centerIn: parent
        spacing: 8

        Button {
            objectName: "nextHunkButton"
            anchors.verticalCenter: parent.verticalCenter
            text: "Next hunk"
        }

        Button {
            objectName: "previousHunkButton"
            anchors.verticalCenter: parent.verticalCenter
            text: "Previous hunk"
        }

        Button {
            objectName: "copyLeftToRightButton"
            anchors.verticalCenter: parent.verticalCenter
            enabled: false
            text: "Copy ->"
        }

        Button {
            objectName: "copyRightToLeftButton"
            anchors.verticalCenter: parent.verticalCenter
            enabled: false
            text: "<- Copy"
        }

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: "F7 / Shift+F7"
        }

        Label {
            objectName: "hunkStatus"
            anchors.verticalCenter: parent.verticalCenter
            text: "Hunk -/0"
        }
    }
}
)qml",
        QUrl());
    std::unique_ptr<QObject> hunk_toolbar_object(hunk_toolbar_component.create());
    if (!hunk_toolbar_object) {
        qFatal("TortoiseDiff Step 12 hunk toolbar failed to load: %s",
            qPrintable(hunk_toolbar_component.errorString()));
    }

    auto* hunk_toolbar = qobject_cast<QQuickItem*>(hunk_toolbar_object.get());
    if (!hunk_toolbar) {
        qFatal("TortoiseDiff Step 12 hunk toolbar is not a QQuickItem.");
    }
    auto* hunk_status = hunk_toolbar->findChild<QObject*>(QStringLiteral("hunkStatus"));
    auto* next_hunk_button = hunk_toolbar->findChild<QObject*>(QStringLiteral("nextHunkButton"));
    auto* previous_hunk_button = hunk_toolbar->findChild<QObject*>(QStringLiteral("previousHunkButton"));
    auto* copy_left_to_right_button = hunk_toolbar->findChild<QObject*>(QStringLiteral("copyLeftToRightButton"));
    auto* copy_right_to_left_button = hunk_toolbar->findChild<QObject*>(QStringLiteral("copyRightToLeftButton"));
    if (!hunk_status || !next_hunk_button || !previous_hunk_button || !copy_left_to_right_button ||
        !copy_right_to_left_button)
    {
        qFatal("TortoiseDiff Step 12 hunk toolbar controls were not found.");
    }
    hunk_toolbar->setParentItem(root);

    QQmlComponent context_menu_component(&qml_engine);
    context_menu_component.setData(R"qml(
import QtQuick

Rectangle {
    id: menu
    color: "#ffffff"
    border.color: "#8c959f"
    visible: false
    z: 30
    width: 232
    height: menuColumn.implicitHeight + 2

    function closeMenu() {
        visible = false
    }

    Column {
        id: menuColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1

        Repeater {
            model: [
                "use other",
                "use both (this one first)",
                "use both (this one last)"
            ]

            delegate: Rectangle {
                width: menuColumn.width
                height: 26
                color: rowMouse.containsMouse ? "#0969da" : "#ffffff"

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    color: rowMouse.containsMouse ? "#ffffff" : "#24292f"
                    text: modelData
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: menu.closeMenu()
                }
            }
        }

        Rectangle {
            width: menuColumn.width
            height: 1
            color: "#d0d7de"
        }

        Rectangle {
            width: menuColumn.width
            height: 26
            color: rowMouse.containsMouse ? "#0969da" : "#ffffff"

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                color: rowMouse.containsMouse ? "#ffffff" : "#24292f"
                text: "use whole other file"
            }

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: menu.closeMenu()
            }
        }
    }
}
)qml",
        QUrl());
    std::unique_ptr<QObject> context_menu_object(context_menu_component.create());
    if (!context_menu_object) {
        qFatal("TortoiseDiff Step 14 context menu failed to load: %s",
            qPrintable(context_menu_component.errorString()));
    }
    auto* context_menu = qobject_cast<QQuickItem*>(context_menu_object.get());
    if (!context_menu) {
        qFatal("TortoiseDiff Step 14 context menu is not a QQuickItem.");
    }
    context_menu->setParentItem(root);

    bool applying_mirrored_scroll = false;
    bool applying_mirrored_zoom = false;
    const auto mirror_scroll = [&](auto&& apply) {
        if (applying_mirrored_scroll) {
            return;
        }

        applying_mirrored_scroll = true;
        apply();
        applying_mirrored_scroll = false;
    };
    const auto mirror_zoom = [&](ScintillaQuick_item& target, int value) {
        if (applying_mirrored_zoom) {
            return;
        }

        applying_mirrored_zoom = true;
        if (target.send(SCI_GETZOOM) != value) {
            target.send(SCI_SETZOOM, value);
        }
        applying_mirrored_zoom = false;
    };

    QQuickItem left_container;
    QQuickItem right_container;
    ScintillaQuick_item left;
    ScintillaQuick_item right;
    left_container.setParentItem(root);
    right_container.setParentItem(root);
    left_container.setClip(true);
    right_container.setClip(true);
    left.setParentItem(&left_container);
    right.setParentItem(&right_container);
    QString left_text;
    QString right_text;
    int shared_horizontal_scroll_width = 1;

    QQmlComponent active_hunk_boundary_component(&qml_engine);
    active_hunk_boundary_component.setData(R"qml(
import QtQuick

Rectangle {
    color: "black"
    enabled: false
    visible: false
    z: 5
}
)qml",
        QUrl());
    const auto create_active_hunk_boundary = [&](QQuickItem& parent) {
        std::unique_ptr<QObject> boundary_object(active_hunk_boundary_component.create());
        if (!boundary_object) {
            qFatal("TortoiseDiff Step 12.1 hunk boundary failed to load: %s",
                qPrintable(active_hunk_boundary_component.errorString()));
        }

        auto* boundary = qobject_cast<QQuickItem*>(boundary_object.get());
        if (!boundary) {
            qFatal("TortoiseDiff Step 12.1 hunk boundary is not a QQuickItem.");
        }
        boundary->setAcceptedMouseButtons(Qt::NoButton);
        boundary->setParentItem(&parent);
        return boundary_object;
    };
    std::unique_ptr<QObject> left_top_hunk_boundary_object = create_active_hunk_boundary(left_container);
    std::unique_ptr<QObject> left_bottom_hunk_boundary_object = create_active_hunk_boundary(left_container);
    std::unique_ptr<QObject> right_top_hunk_boundary_object = create_active_hunk_boundary(right_container);
    std::unique_ptr<QObject> right_bottom_hunk_boundary_object = create_active_hunk_boundary(right_container);
    auto* left_top_hunk_boundary = qobject_cast<QQuickItem*>(left_top_hunk_boundary_object.get());
    auto* left_bottom_hunk_boundary = qobject_cast<QQuickItem*>(left_bottom_hunk_boundary_object.get());
    auto* right_top_hunk_boundary = qobject_cast<QQuickItem*>(right_top_hunk_boundary_object.get());
    auto* right_bottom_hunk_boundary = qobject_cast<QQuickItem*>(right_bottom_hunk_boundary_object.get());

    const auto vertical_scrollbar_metrics = [&left]() {
        const int line_count = std::max(1, static_cast<int>(left.send(SCI_GETLINECOUNT)));
        const int lines_on_screen = std::clamp(static_cast<int>(left.send(SCI_LINESONSCREEN)), 1, line_count);
        const int max_first_line = std::max(0, line_count - lines_on_screen);
        const int first_visible_line =
            std::clamp(static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE)), 0, max_first_line);
        return std::array{first_visible_line, line_count, lines_on_screen, max_first_line};
    };

    const auto horizontal_text_width = [](ScintillaQuick_item& pane) {
        int width = std::max(0, static_cast<int>(pane.width()));
        width -= static_cast<int>(pane.send(SCI_GETMARGINLEFT));
        const int margin_count = static_cast<int>(pane.send(SCI_GETMARGINS));
        for (int margin = 0; margin < margin_count; ++margin) {
            width -= static_cast<int>(pane.send(SCI_GETMARGINWIDTHN, margin));
        }
        width -= static_cast<int>(pane.send(SCI_GETMARGINRIGHT));
        return std::max(1, width);
    };

    const auto measured_text_width = [](ScintillaQuick_item& pane, const QString& text) {
        int max_width = 1;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (QString line : lines) {
            line.replace(QLatin1Char('\t'), QStringLiteral("    "));
            const QByteArray bytes = line.toUtf8();
            max_width =
                std::max(max_width, static_cast<int>(pane.send(SCI_TEXTWIDTH, STYLE_DEFAULT,
                                        reinterpret_cast<sptr_t>(bytes.constData()))));
        }
        return max_width;
    };

    const auto update_shared_horizontal_scroll_width = [&]() {
        shared_horizontal_scroll_width =
            std::max({1, measured_text_width(left, left_text), measured_text_width(right, right_text)});
        if (left.send(SCI_GETSCROLLWIDTH) != shared_horizontal_scroll_width) {
            left.send(SCI_SETSCROLLWIDTH, shared_horizontal_scroll_width);
        }
        if (right.send(SCI_GETSCROLLWIDTH) != shared_horizontal_scroll_width) {
            right.send(SCI_SETSCROLLWIDTH, shared_horizontal_scroll_width);
        }
    };

    const auto horizontal_scrollbar_metrics = [&left, &right, &horizontal_text_width, &shared_horizontal_scroll_width]() {
        const int text_width = std::max(1, shared_horizontal_scroll_width);
        const int viewport_width = std::min(horizontal_text_width(left), horizontal_text_width(right));
        const int max_x_offset = std::max(0, text_width - viewport_width);
        const int x_offset = std::clamp(static_cast<int>(left.send(SCI_GETXOFFSET)), 0, max_x_offset);
        return std::array{x_offset, text_width, viewport_width, max_x_offset};
    };

    const auto refresh_vertical_scrollbar = [&]() {
        const auto [first_visible_line, line_count, lines_on_screen, max_first_line] = vertical_scrollbar_metrics();
        const QSignalBlocker block_scrollbar_signals(vertical_scrollbar);
        const bool visible = line_count > lines_on_screen;
        vertical_scrollbar->setProperty("size", static_cast<double>(lines_on_screen) / line_count);
        vertical_scrollbar->setProperty("position", static_cast<double>(first_visible_line) / line_count);
        vertical_scrollbar->setEnabled(visible);
        vertical_scrollbar->setVisible(visible);
        return visible;
    };

    const auto refresh_horizontal_scrollbar = [&]() {
        const auto horizontal_metrics = horizontal_scrollbar_metrics();
        const int x_offset = horizontal_metrics[0];
        const int text_width = horizontal_metrics[1];
        const int viewport_width = horizontal_metrics[2];
        const int max_x_offset = horizontal_metrics[3];
        const QSignalBlocker block_scrollbar_signals(horizontal_scrollbar);
        const bool visible = text_width > viewport_width;
        horizontal_scrollbar->setProperty("size", std::min(1.0, static_cast<double>(viewport_width) / text_width));
        horizontal_scrollbar->setProperty("position", static_cast<double>(x_offset) / text_width);
        horizontal_scrollbar->setEnabled(visible);
        horizontal_scrollbar->setVisible(visible);
        return max_x_offset > 0;
    };

    QTimer vertical_scrollbar_position_timer;
    vertical_scrollbar_position_timer.setInterval(0);
    vertical_scrollbar_position_timer.setSingleShot(true);
    QObject::connect(vertical_scrollbar, SIGNAL(positionChanged()), &vertical_scrollbar_position_timer, SLOT(start()));
    QObject::connect(&vertical_scrollbar_position_timer, &QTimer::timeout, &window, [&]() {
        const auto [first_visible_line, line_count, lines_on_screen, max_first_line] = vertical_scrollbar_metrics();
        if (max_first_line <= 0) {
            return;
        }

        const int target_line =
            std::clamp(static_cast<int>(vertical_scrollbar->property("position").toDouble() * line_count + 0.5), 0,
                max_first_line);
        if (target_line != first_visible_line) {
            left.scrollVertical(target_line);
        }
    });

    QTimer horizontal_scrollbar_position_timer;
    horizontal_scrollbar_position_timer.setInterval(0);
    horizontal_scrollbar_position_timer.setSingleShot(true);
    QObject::connect(
        horizontal_scrollbar, SIGNAL(positionChanged()), &horizontal_scrollbar_position_timer, SLOT(start()));
    QObject::connect(&horizontal_scrollbar_position_timer, &QTimer::timeout, &window, [&]() {
        const auto [x_offset, text_width, viewport_width, max_x_offset] = horizontal_scrollbar_metrics();
        if (max_x_offset <= 0) {
            return;
        }

        const int target_x_offset =
            std::clamp(static_cast<int>(horizontal_scrollbar->property("position").toDouble() * text_width + 0.5), 0,
                max_x_offset);
        if (target_x_offset != x_offset || right.send(SCI_GETXOFFSET) != target_x_offset) {
            mirror_scroll([&]() {
                if (left.send(SCI_GETXOFFSET) != target_x_offset) {
                    left.scrollHorizontal(target_x_offset);
                }
                if (right.send(SCI_GETXOFFSET) != target_x_offset) {
                    right.scrollHorizontal(target_x_offset);
                }
            });
            refresh_horizontal_scrollbar();
        }
    });

    const auto apply_pane_geometry = [&]() {
        constexpr qreal hunk_toolbar_height = 40.0;
        constexpr qreal separator_width = 8.0;
        constexpr qreal scrollbar_extent = 16.0;
        const qreal root_width = std::max<qreal>(root->width(), 0.0);
        const qreal root_height = std::max<qreal>(root->height(), 0.0);
        const qreal vertical_extent = vertical_scrollbar->isVisible() ? scrollbar_extent : 0.0;
        const qreal horizontal_extent = horizontal_scrollbar->isVisible() ? scrollbar_extent : 0.0;
        const qreal toolbar_height = root_height > hunk_toolbar_height ? hunk_toolbar_height : 0.0;
        const qreal content_width = std::max<qreal>(0.0, root_width - vertical_extent);
        const qreal content_height = std::max<qreal>(0.0, root_height - toolbar_height - horizontal_extent);
        const qreal actual_separator_width = content_width > separator_width ? separator_width : 0.0;
        const qreal panes_width = content_width - actual_separator_width;
        const qreal left_width = panes_width / 2.0;
        const qreal right_width = panes_width - left_width;

        hunk_toolbar->setPosition({0.0, 0.0});
        hunk_toolbar->setSize({root_width, toolbar_height});
        hunk_toolbar->setVisible(toolbar_height > 0.0);

        left_container.setPosition({0.0, toolbar_height});
        left_container.setSize({left_width, content_height});
        right_container.setPosition({left_width + actual_separator_width, toolbar_height});
        right_container.setSize({right_width, content_height});

        left.setPosition({0.0, 0.0});
        left.setSize(left_container.size());
        right.setPosition({0.0, 0.0});
        right.setSize(right_container.size());

        vertical_scrollbar->setPosition({content_width, toolbar_height});
        vertical_scrollbar->setSize({scrollbar_extent, content_height});
        horizontal_scrollbar->setPosition({0.0, toolbar_height + content_height});
        horizontal_scrollbar->setSize({content_width, scrollbar_extent});
    };

    bool applying_layout = false;
    const auto layout_panes = [&]() {
        if (applying_layout) {
            return;
        }

        applying_layout = true;
        for (int pass = 0; pass < 3; ++pass) {
            const bool previous_vertical_visible = vertical_scrollbar->isVisible();
            const bool previous_horizontal_visible = horizontal_scrollbar->isVisible();

            apply_pane_geometry();
            const bool vertical_visible = refresh_vertical_scrollbar();
            const bool horizontal_visible = refresh_horizontal_scrollbar();
            if (vertical_visible == previous_vertical_visible && horizontal_visible == previous_horizontal_visible) {
                break;
            }
        }
        apply_pane_geometry();
        refresh_vertical_scrollbar();
        refresh_horizontal_scrollbar();
        applying_layout = false;
    };

    QObject::connect(root, &QQuickItem::widthChanged, &window, layout_panes);
    QObject::connect(root, &QQuickItem::heightChanged, &window, layout_panes);
    QObject::connect(&left, &ScintillaQuick_item::horizontalRangeChanged, &window, [&](int, int) {
        layout_panes();
    });
    QObject::connect(&right, &ScintillaQuick_item::horizontalRangeChanged, &window, [&](int, int) {
        layout_panes();
    });
    QObject::connect(&left, &ScintillaQuick_item::verticalRangeChanged, &window, [&](int, int) {
        layout_panes();
    });
    QObject::connect(&right, &ScintillaQuick_item::verticalRangeChanged, &window, [&](int, int) {
        layout_panes();
    });
    layout_panes();

    const QFont pane_font = scintillaquick::shared::deterministic_test_font(11);
    DiffWidgetInput input = sample_diff_input();
    left_text = render_side_text(input, true);
    right_text = render_side_text(input, false);
    configure_pane(left, pane_font, left_text);
    configure_pane(right, pane_font, right_text);
    update_shared_horizontal_scroll_width();
    apply_diff_markers(left, input, true);
    apply_diff_markers(right, input, false);
    apply_inline_changed_text_highlights(left, right, input);
    layout_panes();
    const int expected_display_rows = static_cast<int>(input.rows.size());
    if (left.send(SCI_GETLINECOUNT) != expected_display_rows || right.send(SCI_GETLINECOUNT) != expected_display_rows) {
        qFatal("TortoiseDiff Step 9 panes must keep display-buffer line numbers aligned.");
    }
    refresh_vertical_scrollbar();
    refresh_horizontal_scrollbar();

    std::vector<int> hunk_start_rows = hunk_start_display_rows(input);
    int selected_hunk_index = -1;
    int active_first_row = -1;
    int active_end_row = -1;
    bool refreshing_panes = false;
    bool suppress_selection_tracking = false;
    const auto update_active_hunk_boundaries = [&]() {
        const auto hide_boundaries = [&]() {
            for (QQuickItem* boundary : std::array{left_top_hunk_boundary, left_bottom_hunk_boundary,
                     right_top_hunk_boundary, right_bottom_hunk_boundary}) {
                boundary->setVisible(false);
            }
        };
        if (active_first_row < 0 || active_end_row <= active_first_row) {
            hide_boundaries();
            return;
        }

        const int first_row = active_first_row;
        const int end_row = active_end_row;
        const qreal dpr = std::max<qreal>(1.0, window.effectiveDevicePixelRatio());
        constexpr qreal line_height_physical_pixels = 2.0;
        const qreal line_height = line_height_physical_pixels / dpr;
        const auto snap_y = [dpr](qreal y) {
            return std::round(y * dpr) / dpr;
        };
        const auto update_pane_boundaries =
            [&](ScintillaQuick_item& pane, QQuickItem* top_boundary, QQuickItem* bottom_boundary) {
                const int last_row = end_row - 1;
                const sptr_t top_position = pane.send(SCI_POSITIONFROMLINE, first_row);
                const sptr_t bottom_position = pane.send(SCI_POSITIONFROMLINE, last_row);
                const qreal top_y = snap_y(static_cast<qreal>(pane.send(SCI_POINTYFROMPOSITION, 0, top_position)));
                const qreal bottom_y = snap_y(
                    static_cast<qreal>(pane.send(SCI_POINTYFROMPOSITION, 0, bottom_position)) +
                    static_cast<qreal>(std::max(1, static_cast<int>(pane.send(SCI_TEXTHEIGHT, last_row)))));
                const qreal width = std::max<qreal>(0.0, pane.width());

                top_boundary->setPosition({0.0, top_y});
                top_boundary->setSize({width, line_height});
                top_boundary->setVisible(true);

                bottom_boundary->setPosition({0.0, bottom_y - line_height});
                bottom_boundary->setSize({width, line_height});
                bottom_boundary->setVisible(true);
            };
        update_pane_boundaries(left, left_top_hunk_boundary, left_bottom_hunk_boundary);
        update_pane_boundaries(right, right_top_hunk_boundary, right_bottom_hunk_boundary);
    };
    const auto update_hunk_controls = [&]() {
        const bool has_active_range = active_first_row >= 0 && active_end_row > active_first_row;
        hunk_status->setProperty("text", has_active_range ?
                (active_end_row == active_first_row + 1 ?
                        QStringLiteral("Row %1").arg(active_first_row + 1) :
                        QStringLiteral("Rows %1-%2").arg(active_first_row + 1).arg(active_end_row)) :
                QStringLiteral("Rows -"));
        copy_left_to_right_button->setProperty("enabled", has_active_range);
        copy_right_to_left_button->setProperty("enabled", has_active_range);
    };
    const auto set_active_display_row_range = [&](int first_display_row, int end_display_row) {
        const auto [first_row, end_row] =
            exact_display_row_range(first_display_row, end_display_row, static_cast<int>(input.rows.size()));
        active_first_row = first_row;
        active_end_row = end_row;
        selected_hunk_index =
            first_hunk_index_intersecting_display_row_range(input, hunk_start_rows, active_first_row, active_end_row);
        update_hunk_controls();
        update_active_hunk_boundaries();
    };
    const auto set_active_changed_block_from_display_row = [&](int display_row) {
        if (display_row >= active_first_row && display_row < active_end_row) {
            return;
        }

        const auto [first_row, end_row] = changed_block_range_for_display_row(input, hunk_start_rows, display_row);
        set_active_display_row_range(first_row, end_row);
    };
    const auto display_row_from_mouse_event = [](ScintillaQuick_item& pane, QMouseEvent* event) {
        const QPoint point = event->pos();
        const sptr_t position = pane.send(SCI_POSITIONFROMPOINT, point.x(), point.y());
        if (position < 0) {
            return -1;
        }
        return static_cast<int>(pane.send(SCI_LINEFROMPOSITION, position));
    };
    const auto hide_context_menu = [&]() {
        context_menu->setVisible(false);
    };
    const auto show_context_menu = [&](ScintillaQuick_item& pane, QMouseEvent* event) {
        const QPointF root_position = pane.mapToItem(root, event->position());
        const qreal x = std::clamp(root_position.x(), 0.0, std::max<qreal>(0.0, root->width() - context_menu->width()));
        const qreal y =
            std::clamp(root_position.y(), 0.0, std::max<qreal>(0.0, root->height() - context_menu->height()));
        context_menu->setPosition({x, y});
        context_menu->setVisible(true);
    };
    const auto selected_display_row_range = [](ScintillaQuick_item& pane) {
        const sptr_t selection_start = pane.send(SCI_GETSELECTIONSTART);
        const sptr_t selection_end = pane.send(SCI_GETSELECTIONEND);
        const sptr_t last_position = selection_end > selection_start ? selection_end - 1 : selection_start;
        const int first_row = static_cast<int>(pane.send(SCI_LINEFROMPOSITION, selection_start));
        const int end_row = static_cast<int>(pane.send(SCI_LINEFROMPOSITION, last_position)) + 1;
        return std::array{std::min(first_row, end_row), std::max(first_row, end_row)};
    };
    const auto selected_hunk_is_visible = [&]() {
        if (selected_hunk_index < 0 || selected_hunk_index >= static_cast<int>(hunk_start_rows.size())) {
            return false;
        }

        const int first_visible_line = static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE));
        const int lines_on_screen = std::max(1, static_cast<int>(left.send(SCI_LINESONSCREEN)));
        const int hunk_row = hunk_start_rows[static_cast<size_t>(selected_hunk_index)];
        return hunk_row >= first_visible_line && hunk_row < first_visible_line + lines_on_screen;
    };
    const auto hunk_navigation_target = [&](int direction) {
        if (hunk_start_rows.empty()) {
            return -1;
        }

        if (selected_hunk_is_visible()) {
            const int count = static_cast<int>(hunk_start_rows.size());
            return std::clamp(selected_hunk_index + direction, 0, count - 1);
        }

        const int first_visible_line = static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE));
        if (direction > 0) {
            const auto it = std::lower_bound(hunk_start_rows.begin(), hunk_start_rows.end(), first_visible_line);
            return it == hunk_start_rows.end() ? static_cast<int>(hunk_start_rows.size()) - 1 :
                                                 static_cast<int>(it - hunk_start_rows.begin());
        }

        const auto it = std::upper_bound(hunk_start_rows.begin(), hunk_start_rows.end(), first_visible_line);
        if (it == hunk_start_rows.begin()) {
            return 0;
        }
        return static_cast<int>((it - hunk_start_rows.begin()) - 1);
    };
    const auto centered_hunk_first_visible_line = [&](int hunk_row) {
        const int line_count = std::max(1, static_cast<int>(left.send(SCI_GETLINECOUNT)));
        const int lines_on_screen = std::clamp(static_cast<int>(left.send(SCI_LINESONSCREEN)), 1, line_count);
        const int max_first_line = std::max(0, line_count - lines_on_screen);
        return std::clamp(hunk_row - lines_on_screen / 2, 0, max_first_line);
    };
    const auto navigate_hunk = [&](int direction) {
        const int target_index = hunk_navigation_target(direction);
        if (target_index < 0) {
            return;
        }

        selected_hunk_index = target_index;
        const int target_row = hunk_start_rows[static_cast<size_t>(selected_hunk_index)];
        const auto [first_row, end_row] = hunk_display_row_range(input, target_row);
        active_first_row = first_row;
        active_end_row = end_row;
        const int first_visible_line = centered_hunk_first_visible_line(target_row);
        mirror_scroll([&]() {
            left.scrollVertical(first_visible_line);
            right.scrollVertical(first_visible_line);
        });
        refresh_vertical_scrollbar();
        update_hunk_controls();
        update_active_hunk_boundaries();
    };
    const auto refresh_panes_from_input = [&]() {
        const int previous_first_visible_line = static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE));
        const int previous_x_offset = static_cast<int>(left.send(SCI_GETXOFFSET));

        refreshing_panes = true;
        left_text = render_side_text(input, true);
        right_text = render_side_text(input, false);
        configure_pane(left, pane_font, left_text);
        configure_pane(right, pane_font, right_text);
        update_shared_horizontal_scroll_width();
        apply_diff_markers(left, input, true);
        apply_diff_markers(right, input, false);
        apply_inline_changed_text_highlights(left, right, input);
        layout_panes();
        refreshing_panes = false;

        const int line_count = std::max(1, static_cast<int>(left.send(SCI_GETLINECOUNT)));
        const int lines_on_screen = std::clamp(static_cast<int>(left.send(SCI_LINESONSCREEN)), 1, line_count);
        const int max_first_line = std::max(0, line_count - lines_on_screen);
        const auto horizontal_metrics = horizontal_scrollbar_metrics();
        const int max_x_offset = horizontal_metrics[3];
        const int first_visible_line = std::clamp(previous_first_visible_line, 0, max_first_line);
        const int restored_x_offset = std::clamp(previous_x_offset, 0, max_x_offset);

        mirror_scroll([&]() {
            left.scrollVertical(first_visible_line);
            right.scrollVertical(first_visible_line);
            left.scrollHorizontal(restored_x_offset);
            right.scrollHorizontal(restored_x_offset);
        });
        refresh_vertical_scrollbar();
        refresh_horizontal_scrollbar();
    };
    const auto apply_selected_hunk = [&](bool left_to_right) {
        const bool target_left_side = !left_to_right;
        if (!apply_display_row_range_to_target(input, active_first_row, active_end_row, target_left_side)) {
            return;
        }

        validate_diff_input(input);
        hunk_start_rows = hunk_start_display_rows(input);
        if (hunk_start_rows.empty()) {
            selected_hunk_index = -1;
            active_first_row = -1;
            active_end_row = -1;
        } else {
            selected_hunk_index = std::clamp(selected_hunk_index, 0, static_cast<int>(hunk_start_rows.size()) - 1);
            const auto [first_row, end_row] =
                hunk_display_row_range(input, hunk_start_rows[static_cast<size_t>(selected_hunk_index)]);
            active_first_row = first_row;
            active_end_row = end_row;
        }
        refresh_panes_from_input();
        if (selected_hunk_index >= 0) {
            const int target_row = hunk_start_rows[static_cast<size_t>(selected_hunk_index)];
            const int first_visible_line = centered_hunk_first_visible_line(target_row);
            mirror_scroll([&]() {
                left.scrollVertical(first_visible_line);
                right.scrollVertical(first_visible_line);
            });
            refresh_vertical_scrollbar();
        }
        update_hunk_controls();
        update_active_hunk_boundaries();
    };
    const auto handle_hunk_shortcut = [&](QKeyEvent* event) {
        if (event->key() != Qt::Key_F7) {
            return;
        }

        const Qt::KeyboardModifiers allowed_modifiers = Qt::ShiftModifier | Qt::KeypadModifier;
        if (event->modifiers() & ~allowed_modifiers) {
            return;
        }

        navigate_hunk(event->modifiers() & Qt::ShiftModifier ? -1 : 1);
        event->accept();
    };
    QObject::connect(&left, &ScintillaQuick_item::keyPressed, &window, handle_hunk_shortcut);
    QObject::connect(&right, &ScintillaQuick_item::keyPressed, &window, handle_hunk_shortcut);
    const auto install_source_side_copy = [&](ScintillaQuick_item& pane, bool left_side) {
        auto* pane_ptr = &pane;
        QObject::connect(&pane, &ScintillaQuick_item::aboutToCopy, &window, [&, pane_ptr, left_side](QMimeData* data) {
            const int first_display_row =
                static_cast<int>(pane_ptr->send(SCI_LINEFROMPOSITION, pane_ptr->send(SCI_GETSELECTIONSTART)));
            data->setText(source_side_copy_text(data->text(), input.rows, left_side, first_display_row));
        });
    };
    install_source_side_copy(left, true);
    install_source_side_copy(right, false);
    const auto install_active_hunk_tracking = [&](ScintillaQuick_item& pane) {
        auto* pane_ptr = &pane;
        QObject::connect(&pane, &ScintillaQuick_item::buttonPressed, &window, [&, pane_ptr](QMouseEvent* event) {
            if (event->button() != Qt::RightButton) {
                hide_context_menu();
                return;
            }

            suppress_selection_tracking = true;
            const int display_row = display_row_from_mouse_event(*pane_ptr, event);
            if (display_row >= 0) {
                set_active_changed_block_from_display_row(display_row);
            }
            show_context_menu(*pane_ptr, event);
        });
        QObject::connect(&pane, &ScintillaQuick_item::buttonReleased, &window, [&, pane_ptr](QMouseEvent* event) {
            if (event->button() == Qt::RightButton) {
                const int display_row = display_row_from_mouse_event(*pane_ptr, event);
                if (display_row >= 0) {
                    set_active_changed_block_from_display_row(display_row);
                }
                show_context_menu(*pane_ptr, event);
                suppress_selection_tracking = false;
                return;
            }
            if (event->button() != Qt::LeftButton) {
                return;
            }

            const auto [first_row, end_row] = selected_display_row_range(*pane_ptr);
            set_active_display_row_range(first_row, end_row);
        });
        QObject::connect(&pane, &ScintillaQuick_item::updateUi, &window, [&, pane_ptr](Scintilla::Update updated) {
            if (refreshing_panes || suppress_selection_tracking ||
                !(static_cast<int>(updated) & static_cast<int>(Scintilla::Update::Selection)))
            {
                return;
            }

            const auto [first_row, end_row] = selected_display_row_range(*pane_ptr);
            set_active_display_row_range(first_row, end_row);
        });
    };
    install_active_hunk_tracking(left);
    install_active_hunk_tracking(right);
    CallbackEventFilter next_hunk_click_filter([&]() { navigate_hunk(1); });
    CallbackEventFilter previous_hunk_click_filter([&]() { navigate_hunk(-1); });
    CallbackEventFilter copy_left_to_right_click_filter([&]() { apply_selected_hunk(true); });
    CallbackEventFilter copy_right_to_left_click_filter([&]() { apply_selected_hunk(false); });
    next_hunk_button->installEventFilter(&next_hunk_click_filter);
    previous_hunk_button->installEventFilter(&previous_hunk_click_filter);
    copy_left_to_right_button->installEventFilter(&copy_left_to_right_click_filter);
    copy_right_to_left_button->installEventFilter(&copy_right_to_left_click_filter);
    update_hunk_controls();
    update_active_hunk_boundaries();

    QObject::connect(&left, &ScintillaQuick_item::verticalScrolled, &right, [&](int value) {
        mirror_scroll([&]() {
            right.scrollVertical(value);
        });
        update_active_hunk_boundaries();
    });
    QObject::connect(&right, &ScintillaQuick_item::verticalScrolled, &left, [&](int value) {
        mirror_scroll([&]() {
            left.scrollVertical(value);
        });
        update_active_hunk_boundaries();
    });
    QObject::connect(&left, &ScintillaQuick_item::horizontalScrolled, &right, [&](int value) {
        mirror_scroll([&]() {
            right.scrollHorizontal(value);
        });
    });
    QObject::connect(&right, &ScintillaQuick_item::horizontalScrolled, &left, [&](int value) {
        mirror_scroll([&]() {
            left.scrollHorizontal(value);
        });
    });
    QObject::connect(&left, &ScintillaQuick_item::verticalScrolled, vertical_scrollbar, [&](int) {
        refresh_vertical_scrollbar();
        update_active_hunk_boundaries();
    });
    QObject::connect(&left, &ScintillaQuick_item::horizontalScrolled, horizontal_scrollbar, [&](int) {
        refresh_horizontal_scrollbar();
    });
    QObject::connect(&left, &ScintillaQuick_item::zoom, &right, [&](int value) {
        mirror_zoom(right, value);
    });
    QObject::connect(&right, &ScintillaQuick_item::zoom, &left, [&](int value) {
        mirror_zoom(left, value);
        update_shared_horizontal_scroll_width();
        layout_panes();
        update_active_hunk_boundaries();
    });
    QObject::connect(&left, &ScintillaQuick_item::zoom, vertical_scrollbar, [&](int) {
        update_shared_horizontal_scroll_width();
        layout_panes();
        update_active_hunk_boundaries();
    });
    QObject::connect(root, &QQuickItem::widthChanged, &window, update_active_hunk_boundaries);
    QObject::connect(root, &QQuickItem::heightChanged, &window, update_active_hunk_boundaries);
    QObject::connect(&left, &ScintillaQuick_item::verticalRangeChanged, &window, [&](int, int) {
        update_active_hunk_boundaries();
    });
    QObject::connect(&right, &ScintillaQuick_item::verticalRangeChanged, &window, [&](int, int) {
        update_active_hunk_boundaries();
    });

    window.show();
    left.forceActiveFocus();

    return app.exec();
}
