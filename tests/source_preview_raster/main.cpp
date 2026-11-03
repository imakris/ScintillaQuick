// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_source_preview_raster_p.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cstdio>
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
    // A fresh worker for each source line cannot reuse any previous text,
    // formats or row raster. Composite every contribution in document order
    // through Qt's source-over painter, independently of the worker's loop.
    QImage expected(config.pixels, QImage::Format_ARGB32_Premultiplied);
    expected.fill(config.background);
    auto transparent = config;
    transparent.background = Qt::transparent;
    QPainter painter(&expected);
    for (const auto& line : lines) {
        painter.drawImage(0, 0, fresh(transparent, {line}));
    }
    return expected;
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
    require(complete == independent_rows(config, lines), "dense rows lost, reordered or reused wrong styles");
    require(probe.render(config, lines, 1) == complete, "chunk boundaries changed the raster");

    const auto repeated = styled_line("MMMM 123", 0, 1);
    require(fresh(config, {repeated}) != fresh(config, {repeated, repeated}),
        "repeated rows must each contribute glyph coverage");
    require(fresh(config, {styled_line("text", 0, 1)}) !=
        fresh(config, {styled_line("text", 0, 2)}), "style changes did not affect the raster");
    const auto clipped = styled_line(QByteArray("prefix ") + QByteArray::fromHex("f09f98"), 0);
    require(fresh(config, {clipped}) == fresh(config, {styled_line("prefix ", 0)}),
        "a clipped UTF-8 tail invented a glyph");
    require(fresh(config, {styled_line("text\r\n", 0)}) == fresh(config, {styled_line("text", 0)}),
        "line endings changed visible glyph coverage");

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
        std::puts("PASS source preview raster: styles, Unicode, all-row coverage, chunks, configuration and cancellation");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL source preview raster: %s\n", error.what());
        return 1;
    }
}
