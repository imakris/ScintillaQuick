// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_source_preview_raster_p.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace preview = ScintillaQuick_preview;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

preview::Line styled_line(const QByteArray& text, int number, int style = 0)
{
    preview::Line line;
    line.number = number;
    line.styled_bytes.resize(text.size() * 2);
    for (qsizetype index = 0; index < text.size(); ++index) {
        line.styled_bytes[index * 2] = text[index];
        line.styled_bytes[index * 2 + 1] = char(style);
    }
    return line;
}

preview::Configuration configuration()
{
    preview::Configuration config;
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(9.75);
    for (auto& style : config.styles) {
        style.font = font;
        style.foreground = QColor(220, 225, 230);
    }
    config.styles[1].font.setBold(true);
    config.styles[1].foreground = QColor(240, 80, 65);
    config.styles[2].font.setItalic(true);
    config.styles[2].foreground = QColor(70, 220, 140);
    config.pixels = QSize(92, 16);
    config.background = QColor(25, 28, 35);
    config.line_advance = 0.25;
    config.line_count = 64;
    return config;
}

class Probe
{
public:
    Probe()
    {
        QObject::connect(&worker, &preview::Raster_worker::chunk_finished, &worker,
            [this](quint64 serial, bool final, const QImage& image, bool failed) {
                require(!failed, "raster allocation failed");
                ++acknowledgments;
                received_serial = serial;
                completed = final;
                result = image;
            });
    }

    QImage render(const preview::Configuration& config, const QVector<preview::Line>& lines,
        int chunk_size = preview::k_chunk_lines)
    {
        const quint64 serial = ++*generation;
        worker.begin(config, serial);
        for (qsizetype offset = 0; offset < lines.size(); offset += chunk_size) {
            const qsizetype count = std::min<qsizetype>(chunk_size, lines.size() - offset);
            const bool final = offset + count == lines.size();
            const int before = acknowledgments;
            worker.consume({lines.sliced(offset, count), serial, final});
            require(acknowledgments == before + 1 && received_serial == serial,
                "chunk acknowledgment lost its serial");
            require(completed == final && result.isNull() == !final,
                "partial chunk published a raster, or final chunk did not");
        }
        require(result.size() == config.pixels, "completed raster has the wrong size");
        return result;
    }

    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(0);
    preview::Raster_worker worker{generation};
    QImage result;
    quint64 received_serial = 0;
    int acknowledgments = 0;
    bool completed = false;
};

QImage fresh(const preview::Configuration& config, const QVector<preview::Line>& lines)
{
    Probe probe;
    return probe.render(config, lines);
}

QImage independent_rows(const preview::Configuration& config, const QVector<preview::Line>& lines)
{
    // Separate logical rows have disjoint coverage. Render them independently
    // at one pixel per line, then let Qt area-average that grid instead of
    // repeating the worker's fractional placement and accumulation.
    auto logical = config;
    logical.pixels.setHeight(config.line_count);
    logical.line_advance = 1;
    QImage expected(logical.pixels, QImage::Format_ARGB32_Premultiplied);
    expected.fill(config.background);
    auto transparent = logical;
    transparent.background = Qt::transparent;
    QPainter painter(&expected);
    for (const auto& line : lines) {
        painter.drawImage(0, 0, fresh(transparent, {line}));
    }
    painter.end();
    return expected.scaled(config.pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

bool matching_pixels(const QImage& left, const QImage& right, int tolerance)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QRgb a = left.pixel(x, y);
            const QRgb b = right.pixel(x, y);
            if (qAbs(qRed(a)   - qRed(b))   > tolerance ||
                qAbs(qGreen(a) - qGreen(b)) > tolerance ||
                qAbs(qBlue(a)  - qBlue(b))  > tolerance ||
                qAbs(qAlpha(a) - qAlpha(b)) > tolerance)
            {
                return false;
            }
        }
    }
    return true;
}

void check_fractional_coverage()
{
    auto config = configuration();
    // The reported 125% display scale gives 115 physical pixels for a
    // 92-logical-pixel source strip. Exercise both sides of one pixel per line.
    config.pixels = QSize(115, 1);
    config.background = Qt::transparent;
    config.line_advance = 1;
    config.line_count = 1;
    const QByteArray text("MMMM uniform text 123; // fractional coverage");
    const QImage row = fresh(config, {styled_line(text, 0)});
    bool has_coverage = false;
    for (int x = 0; x < row.width(); ++x) {
        has_coverage |= qAlpha(row.pixel(x, 0)) > 0;
    }
    require(has_coverage, "fractional coverage fixture has no glyphs");

    config.line_count = 599;
    QVector<preview::Line> lines;
    for (int number = 0; number < config.line_count; ++number) {
        lines.append(styled_line(text, number));
    }
    Probe probe;
    for (int height : {493, 603}) {
        config.pixels.setHeight(height);
        config.line_advance = (qreal)height / config.line_count;
        const QImage complete = probe.render(config, lines);
        // Equal source rows tile the entire output. Area conservation makes
        // every physical row equal, including the final fractional boundary.
        for (int y = 0; y < complete.height(); ++y) {
            for (int x = 0; x < complete.width(); ++x) {
                require(complete.pixel(x, y) == row.pixel(x, 0),
                    "fractional placement introduced bands or lost final-row coverage");
            }
        }
        require(probe.render(config, lines, 1) == complete,
            "a chunk boundary changed pending fractional-row coverage");
    }
}

void check_occurrence_marks()
{
    auto config = configuration();
    config.columns = 20;
    config.pixels = QSize(200, 16);
    config.line_advance = 1;
    constexpr int edge_width = 3;

    // A single letter before the tab still reaches the same first tab stop.
    // Its UTF-8 byte length must not shift either subsequent word marker.
    const QByteArray ascii("A\tword  word");
    const QByteArray utf8 = QStringLiteral("é\tword  word").toUtf8();
    const auto marks_for = [&](const QByteArray& text) {
        const int first = int(text.indexOf("word"));
        const int last = int(text.lastIndexOf("word"));
        return preview::Raster_worker::occurrence_marks(config,
            {{styled_line(text, 3), {{first, first + 4}, {last, last + 4}}}}, edge_width);
    };
    const auto marks = marks_for(ascii);
    require(marks.size() == 2 && marks[0].top() == 3 && marks[1].top() == 3 &&
        marks[0].right() < marks[1].left(), "matches on one line lost their separate positions");
    require(marks_for(utf8) == marks, "a UTF-8 prefix shifted a word marker");
    config.tab_width = 8;
    const auto wider_tabs = marks_for(utf8);
    require(wider_tabs.size() == 2 && wider_tabs[0].left() > marks[0].left() &&
        wider_tabs[1].left() > marks[1].left(), "word markers did not follow the tab stops");

    // Within the Hebrew run, the earlier logical word is visually on the right.
    const QByteArray mixed = QStringLiteral("A א בגד").toUtf8();
    const int separator = int(mixed.lastIndexOf(' '));
    const auto hebrew = preview::Raster_worker::occurrence_marks(config,
        {{styled_line(mixed, 4), {{2, separator}, {separator + 1, int(mixed.size())}}}}, edge_width);
    require(hebrew.size() == 2 && hebrew[0].left() > hebrew[1].right(),
        "word markers in a right-to-left run lost their visual order");

    // The preview width is expressed in M advances. The first range crosses
    // that boundary and the second starts well beyond it.
    const auto clipped = preview::Raster_worker::occurrence_marks(config,
        {{styled_line(QByteArray(32, 'M'), 6), {{17, 22}, {26, 31}}}}, edge_width);
    const int edge = config.pixels.width();
    require(clipped.size() == 2 && clipped[0].right() == edge - 1 &&
        clipped[0].left() > edge / 2 && clipped[0].width() > edge_width,
        "a partially visible mark did not reach the clipping edge");
    require(clipped[1] == QRect(edge - edge_width, 6, edge_width, 1),
        "a fully clipped match lost the right-edge marker");
}

void check_raster_contract()
{
    auto config = configuration();
    const QByteArray unicode = QStringLiteral("\tint café = α; // 雪😀 é").toUtf8();
    QVector<preview::Line> lines;
    for (int number = 0; number < 48; ++number) {
        auto line = styled_line(number % 7 == 0 ? QByteArray("\r\n") : unicode, number, number % 3);
        if (number % 7 != 0) {
            // Style transitions include UTF-8 continuation bytes; the leading
            // byte, not an incidental tail-byte style, owns each character.
            for (qsizetype byte = 1; byte < unicode.size(); byte += 5) {
                line.styled_bytes[byte * 2 + 1] = char((number + 1) % 3);
            }
        }
        lines.append(std::move(line));
    }
    Probe probe;
    const QImage complete = probe.render(config, lines);
    // The independent path rounds once before averaging and once afterwards;
    // two 8-bit channel steps bound that difference from rounding only at flush.
    require(matching_pixels(complete, independent_rows(config, lines), 2),
        "dense rows lost area coverage or reused wrong styles");
    require(probe.render(config, lines, 1) == complete, "chunk boundaries changed the raster");

    const auto repeated = styled_line("MMMM 123", 0, 1);
    require(fresh(config, {repeated}) != fresh(config, {repeated, styled_line("MMMM 123", 1, 1)}),
        "repeated rows must each contribute glyph coverage");
    require(fresh(config, {styled_line("text", 0, 1)}) !=
        fresh(config, {styled_line("text", 0, 2)}), "style changes did not affect the raster");
    const auto clipped = styled_line(QByteArray("prefix ") + QByteArray::fromHex("f09f98"), 0);
    require(fresh(config, {clipped}) == fresh(config, {styled_line("prefix ", 0)}),
        "a clipped UTF-8 tail invented a glyph");
    require(fresh(config, {styled_line("text\r\n", 0)}) == fresh(config, {styled_line("text", 0)}),
        "line endings changed visible glyph coverage");

    const std::pair<const char*, const char*> decoding_cases[] = {
        {"80", "efbfbd"},                 // Isolated continuation byte.
        {"e228a1", "efbfbd28efbfbd"},     // Invalid continuation around '('.
        {"c0af", "efbfbdefbfbd"},         // Overlong encoding, one replacement per byte.
        {"efbbbf", ""}                    // The existing fragment decoder skips a BOM.
    };
    for (const auto& [bytes, expected] : decoding_cases) {
        require(fresh(config, {styled_line(QByteArray("A") + QByteArray::fromHex(bytes) + "B", 0)}) ==
            fresh(config, {styled_line(QByteArray("A") + QByteArray::fromHex(expected) + "B", 0)}),
            "malformed UTF-8 or embedded BOM changed decoding");
    }

    for (int change = 0; change < 5; ++change) {
        if (change == 0) { config.styles[1].foreground = Qt::blue; }
        if (change == 1) {
            for (auto& style : config.styles) { style.font.setPointSizeF(13); }
        }
        if (change == 2) { config.tab_width = 8; }
        if (change == 3) { config.pixels.setWidth(137); }
        if (change == 4) {
            config.pixels.setHeight(160);
            config.line_advance = 3;
        }
        require(probe.render(config, lines, 3) == fresh(config, lines),
            "new configuration reused stale raster data");
    }

    const QImage retained = probe.result;
    const QImage retained_pixels = retained.copy();
    const quint64 cancelled = ++*probe.generation;
    probe.worker.begin(config, cancelled);
    probe.worker.consume({{repeated}, cancelled, false});
    ++*probe.generation;
    probe.worker.consume({{repeated}, cancelled, true});
    require(probe.received_serial == cancelled && !probe.completed && probe.result.isNull(),
        "cancelled generation published a completed image");
    config.background = Qt::white;
    require(probe.render(config, lines) == fresh(config, lines), "cancelled raster leaked into a new generation");
    require(retained == retained_pixels, "worker reuse damaged a retained publication");
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        check_raster_contract();
        check_fractional_coverage();
        check_occurrence_marks();
        std::puts("PASS source preview raster: styles, Unicode, area coverage, fractional rows, chunks and cancellation");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL source preview raster: %s\n", error.what());
        return 1;
    }
}
