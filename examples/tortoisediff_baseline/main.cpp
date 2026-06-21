// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include <QColor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QStringList>

#include "scintillaquick_font.h"
#include "Scintilla.h"

namespace
{

constexpr int rgb(int red, int green, int blue)
{
    return red | (green << 8) | (blue << 16);
}

constexpr int k_editor_foreground = rgb(31, 35, 40);
constexpr int k_editor_background = rgb(255, 255, 255);
constexpr int k_margin_foreground = rgb(87, 96, 106);
constexpr int k_margin_background = rgb(246, 248, 250);
constexpr int k_selection_background = rgb(188, 214, 253);
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

class Scroll_button final : public QQuickPaintedItem
{
  public:
    enum class Direction
    {
        Up,
        Down,
        Left,
        Right
    };

    Scroll_button(Direction direction, std::function<void()> clicked, QQuickItem* parent = nullptr)
        : QQuickPaintedItem(parent), m_direction(direction), m_clicked(std::move(clicked))
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setAntialiasing(false);
        setOpaquePainting(true);
        setZ(10.0);
    }

    void paint(QPainter* painter) override
    {
        painter->fillRect(boundingRect(), m_pressed ? QColor(54, 63, 74) : QColor(72, 83, 96));
        painter->setPen(QColor(218, 225, 232));
        painter->drawRect(boundingRect().adjusted(0.5, 0.5, -0.5, -0.5));
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 255, 255));
        painter->drawPolygon(arrow_shape());
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        m_pressed = true;
        update();
        if (m_clicked) {
            m_clicked();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressed = false;
            update();
            event->accept();
            return;
        }

        event->ignore();
    }

  private:
    QPolygonF arrow_shape() const
    {
        const qreal edge = std::min(width(), height()) * 0.28;
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;

        switch (m_direction) {
            case Direction::Up:
                return {{cx, cy - edge}, {cx - edge, cy + edge}, {cx + edge, cy + edge}};
            case Direction::Down:
                return {{cx, cy + edge}, {cx - edge, cy - edge}, {cx + edge, cy - edge}};
            case Direction::Left:
                return {{cx - edge, cy}, {cx + edge, cy - edge}, {cx + edge, cy + edge}};
            case Direction::Right:
                return {{cx + edge, cy}, {cx - edge, cy - edge}, {cx - edge, cy + edge}};
        }

        return {};
    }

    Direction m_direction;
    std::function<void()> m_clicked;
    bool m_pressed = false;
};

class Row_tint_overlay final : public QQuickPaintedItem
{
  public:
    Row_tint_overlay(
        ScintillaQuick_item& pane, const DiffWidgetInput& input, bool left_side, QQuickItem* parent = nullptr)
        : QQuickPaintedItem(parent), m_pane(pane), m_input(input), m_left_side(left_side)
    {
        setAcceptedMouseButtons(Qt::NoButton);
        setAntialiasing(false);
        setOpaquePainting(false);
        setZ(2.0);
    }

    void paint(QPainter* painter) override
    {
        const int first_line = static_cast<int>(m_pane.send(SCI_GETFIRSTVISIBLELINE));
        const int line_height = std::max(1, static_cast<int>(m_pane.send(SCI_TEXTHEIGHT, first_line)));
        const int visible_rows = static_cast<int>(height() / line_height) + 2;
        const int last_line = std::min<int>(static_cast<int>(m_input.rows.size()), first_line + visible_rows);

        for (int row_index = std::max(0, first_line); row_index < last_line; ++row_index) {
            const DiffRow& row = m_input.rows[static_cast<size_t>(row_index)];
            const DiffSideState state = m_left_side ? row.leftState : row.rightState;
            const QColor color = overlay_color(state);
            if (!color.isValid()) {
                continue;
            }

            painter->fillRect(QRectF(0.0, (row_index - first_line) * line_height, width(), line_height), color);
        }
    }

  private:
    static QColor overlay_color(DiffSideState state)
    {
        switch (state) {
            case DiffSideState::Added:
                return QColor(122, 216, 132, 96);
            case DiffSideState::Deleted:
                return QColor(245, 127, 127, 96);
            case DiffSideState::Changed:
                return QColor(245, 209, 86, 88);
            case DiffSideState::Filler:
                return QColor(136, 155, 183, 92);
            case DiffSideState::Equal:
                return {};
        }

        return {};
    }

    ScintillaQuick_item& m_pane;
    const DiffWidgetInput& m_input;
    bool m_left_side = false;
};

QStringList source_lines_from_text(const QString& text)
{
    return text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
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
            qFatal("TortoiseDiff Step 7 fixture hunk has %d left/%d right lines, expected %d left/%d right.",
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
        const QString& diff_line = diff_lines[line_index];
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
                    qFatal("TortoiseDiff Step 7 fixture supports one unified-diff file only.");
                }
                saw_file_header = true;
                continue;
            }
            if (diff_line.startsWith(QStringLiteral("index ")) || diff_line.startsWith(QStringLiteral("--- ")) ||
                diff_line.startsWith(QStringLiteral("+++ ")))
            {
                continue;
            }

            qFatal("TortoiseDiff Step 7 fixture has an unexpected line outside a hunk: %s",
                qPrintable(diff_line));
        }

        if (diff_line == QStringLiteral("\\ No newline at end of file")) {
            continue;
        }
        if (diff_line.isEmpty()) {
            qFatal("TortoiseDiff Step 7 fixture has a hunk body line without a diff prefix.");
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
            qFatal("TortoiseDiff Step 7 fixture has an unsupported hunk body prefix: %s",
                qPrintable(diff_line.left(1)));
        }
    }

    finish_hunk();
    if (!saw_hunk) {
        qFatal("TortoiseDiff Step 7 fixture must contain at least one hunk.");
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
            qFatal("TortoiseDiff Step 7 widget input has invalid %s source line %d; expected %d.", side, source_line,
                expected_source_line);
        }
        ++expected_source_line;
    }

    if (expected_source_line - 1 != source_line_count) {
        qFatal("TortoiseDiff Step 7 widget input has %d %s source lines but references %d.", source_line_count, side,
            expected_source_line - 1);
    }
}

void validate_diff_input(const DiffWidgetInput& input)
{
    if (input.rows.empty()) {
        qFatal("TortoiseDiff Step 7 widget input must not be empty.");
    }

    validate_source_line_references(input.rows, true, display_row_count(input.leftText), "left");
    validate_source_line_references(input.rows, false, display_row_count(input.rightText), "right");

    for (const DiffRow& row : input.rows) {
        if (!source_and_state_match(row.leftSourceLine, row.leftState) ||
            !source_and_state_match(row.rightSourceLine, row.rightState))
        {
            qFatal("TortoiseDiff Step 7 widget input has filler state/source-line mismatch.");
        }
    }

    const QString left_display_text = render_side_text(input, true);
    const QString right_display_text = render_side_text(input, false);
    const int expected_rows = static_cast<int>(input.rows.size());
    if (display_row_count(left_display_text) != expected_rows || display_row_count(right_display_text) != expected_rows) {
        qFatal("TortoiseDiff Step 7 widget input display buffers must have the same display row count as the row model.");
    }
    if (left_display_text == right_display_text) {
        qFatal("TortoiseDiff Step 7 widget input must render non-identical pane text.");
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
        QStringLiteral(" // This deliberately long line stays unwrapped in Step 7 so horizontal scrolling can be "
                       "checked while left/right display rows are aligned with blank filler buffer lines.")};

    for (int row = 1; row <= 80; ++row) {
        lines.append(QStringLiteral(" unchanged display row %1 keeps vertical synchronization checkable.")
                         .arg(row, 2, 10, QLatin1Char('0')));
    }

    lines.append(QStringLiteral(" // End of synchronized scrolling fixture."));
    return lines.join(QStringLiteral("\n"));
}

DiffWidgetInput sample_diff_input()
{
    DiffWidgetInput input = diff_input_from_unified_diff(sample_unified_diff_fixture());
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
    // gutters and copy filtering are intentionally left for later steps.
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
    pane.send(SCI_SETCARETFORE, k_editor_background);
    pane.send(SCI_SETCARETWIDTH, 0);

    pane.setProperty("text", text);
    pane.setProperty("readonly", true);
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
    window.setTitle(QStringLiteral("ScintillaQuick TortoiseDiff Step 7 - Unified Diff Fixture"));
    window.resize(1200, 720);
    window.setColor(QColor(214, 219, 225));

    auto* root = window.contentItem();

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
    Row_tint_overlay* left_overlay = nullptr;
    Row_tint_overlay* right_overlay = nullptr;
    left_container.setParentItem(root);
    right_container.setParentItem(root);
    left.setParentItem(&left_container);
    right.setParentItem(&right_container);

    const auto scroll_vertical_by = [&left](int delta_lines) {
        const int current_line = static_cast<int>(left.send(SCI_GETFIRSTVISIBLELINE));
        left.scrollVertical(std::max(0, current_line + delta_lines));
    };
    const auto scroll_horizontal_by = [&left](int delta_columns) {
        const int char_width =
            std::max(1, static_cast<int>(left.send(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("X"))));
        const int current_x = static_cast<int>(left.send(SCI_GETXOFFSET));
        left.scrollHorizontal(std::max(0, current_x + delta_columns * char_width));
    };

    Scroll_button scroll_up(Scroll_button::Direction::Up, [&]() {
        scroll_vertical_by(-3);
    });
    Scroll_button scroll_down(Scroll_button::Direction::Down, [&]() {
        scroll_vertical_by(3);
    });
    Scroll_button scroll_left(Scroll_button::Direction::Left, [&]() {
        scroll_horizontal_by(-8);
    });
    Scroll_button scroll_right(Scroll_button::Direction::Right, [&]() {
        scroll_horizontal_by(8);
    });
    scroll_up.setParentItem(root);
    scroll_down.setParentItem(root);
    scroll_left.setParentItem(root);
    scroll_right.setParentItem(root);

    const auto layout_panes = [&]() {
        constexpr qreal separator_width = 8.0;
        constexpr qreal control_size = 24.0;
        constexpr qreal control_gap = 4.0;
        constexpr qreal control_inset = 10.0;
        const qreal root_width = std::max<qreal>(root->width(), 0.0);
        const qreal root_height = std::max<qreal>(root->height(), 0.0);
        const qreal actual_separator_width = root_width > separator_width ? separator_width : 0.0;
        const qreal panes_width = root_width - actual_separator_width;
        const qreal left_width = panes_width / 2.0;
        const qreal right_width = panes_width - left_width;

        left_container.setPosition({0.0, 0.0});
        left_container.setSize({left_width, root_height});
        right_container.setPosition({left_width + actual_separator_width, 0.0});
        right_container.setSize({right_width, root_height});

        left.setPosition({0.0, 0.0});
        left.setSize(left_container.size());
        right.setPosition({0.0, 0.0});
        right.setSize(right_container.size());
        if (left_overlay) {
            left_overlay->setPosition({0.0, 0.0});
            left_overlay->setSize(left_container.size());
        }
        if (right_overlay) {
            right_overlay->setPosition({0.0, 0.0});
            right_overlay->setSize(right_container.size());
        }

        const qreal control_x = std::max<qreal>(0.0, root_width - control_inset - control_size);
        const qreal bottom_y = std::max<qreal>(0.0, root_height - control_inset - control_size);

        scroll_up.setPosition({control_x, control_inset});
        scroll_down.setPosition({control_x, control_inset + control_size + control_gap});
        scroll_left.setPosition({std::max<qreal>(0.0, control_x - control_size - control_gap), bottom_y});
        scroll_right.setPosition({control_x, bottom_y});

        scroll_up.setSize({control_size, control_size});
        scroll_down.setSize({control_size, control_size});
        scroll_left.setSize({control_size, control_size});
        scroll_right.setSize({control_size, control_size});
    };

    QObject::connect(root, &QQuickItem::widthChanged, &window, layout_panes);
    QObject::connect(root, &QQuickItem::heightChanged, &window, layout_panes);
    layout_panes();

    const QFont pane_font = scintillaquick::shared::deterministic_test_font(11);
    const DiffWidgetInput input = sample_diff_input();
    configure_pane(left, pane_font, render_side_text(input, true));
    configure_pane(right, pane_font, render_side_text(input, false));
    Row_tint_overlay left_overlay_item(left, input, true, &left_container);
    Row_tint_overlay right_overlay_item(right, input, false, &right_container);
    left_overlay = &left_overlay_item;
    right_overlay = &right_overlay_item;
    layout_panes();
    const int expected_display_rows = static_cast<int>(input.rows.size());
    if (left.send(SCI_GETLINECOUNT) != expected_display_rows || right.send(SCI_GETLINECOUNT) != expected_display_rows) {
        qFatal("TortoiseDiff Step 7 panes must keep display-buffer line numbers aligned.");
    }

    const auto update_overlays = [&]() {
        if (left_overlay) {
            left_overlay->update();
        }
        if (right_overlay) {
            right_overlay->update();
        }
    };

    QObject::connect(&left, &ScintillaQuick_item::verticalScrolled, &right, [&](int value) {
        mirror_scroll([&]() {
            right.scrollVertical(value);
        });
        update_overlays();
    });
    QObject::connect(&right, &ScintillaQuick_item::verticalScrolled, &left, [&](int value) {
        mirror_scroll([&]() {
            left.scrollVertical(value);
        });
        update_overlays();
    });
    QObject::connect(&left, &ScintillaQuick_item::horizontalScrolled, &right, [&](int value) {
        mirror_scroll([&]() {
            right.scrollHorizontal(value);
        });
        update_overlays();
    });
    QObject::connect(&right, &ScintillaQuick_item::horizontalScrolled, &left, [&](int value) {
        mirror_scroll([&]() {
            left.scrollHorizontal(value);
        });
        update_overlays();
    });
    QObject::connect(&left, &ScintillaQuick_item::zoom, &right, [&](int value) {
        mirror_zoom(right, value);
        update_overlays();
    });
    QObject::connect(&right, &ScintillaQuick_item::zoom, &left, [&](int value) {
        mirror_zoom(left, value);
        update_overlays();
    });

    window.show();
    left.forceActiveFocus();

    return app.exec();
}
