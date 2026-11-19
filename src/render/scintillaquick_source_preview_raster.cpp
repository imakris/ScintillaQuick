// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_source_preview_raster_p.h"

#include <Scintilla.h>

#include <QCache>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ScintillaQuick_preview {
namespace {

constexpr int k_supersampling = 4;
constexpr int k_strip_cache_bytes = 256 * 1024;
constexpr int k_strip_cache_minimum_cost = 512;
struct Styled_text
{
    QString text;
    QList<QTextLayout::FormatRange> formats;
};

struct coverage_t
{
    qreal red   = 0;
    qreal green = 0;
    qreal blue  = 0;
    qreal alpha = 0;
};

using Style_formats = std::array<QTextCharFormat, k_style_count>;

Styled_text decode_line(const QByteArray& pairs, const Configuration& configuration, Style_formats& formats,
    QVector<int>* byte_cursors = nullptr)
{
    QByteArray bytes(pairs.size() / 2, Qt::Uninitialized);
    auto* utf8 = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        utf8[i] = pairs[i * 2];
    }

    Styled_text decoded;
    decoded.text.reserve(bytes.size());
    if (byte_cursors) {
        byte_cursors->resize(bytes.size() + 1);
    }
    qsizetype i = 0;
    int previous_style = -1;
    while (i < bytes.size()) {
        const auto lead = (unsigned char)utf8[i];
        if (lead == '\r' || lead == '\n') {
            break;
        }

        const int style = (unsigned char)pairs[i * 2 + 1];
        int count = 1;
        if (lead < 0x80) {
            // Keep ASCII tabs/controls intact while avoiding a temporary
            // QString for each byte. Non-ASCII decoding below is unchanged.
            while (i + count < bytes.size()) {
                const auto next = (unsigned char)utf8[i + count];
                if (next >= 0x80 || next == '\r' || next == '\n' ||
                    (unsigned char)pairs[(i + count) * 2 + 1] != style)
                {
                    break;
                }
                ++count;
            }
        }
        else {
            if (lead >= 0xc2 && lead <= 0xdf) { count = 2; } else
            if (lead >= 0xe0 && lead <= 0xef) { count = 3; } else
            if (lead >= 0xf0 && lead <= 0xf4) { count = 4; }

            bool valid_tail = true;
            for (int tail = 1; tail < count && i + tail < bytes.size(); ++tail) {
                valid_tail &= ((unsigned char)utf8[i + tail] & 0xc0) == 0x80;
            }
            if (!valid_tail) {
                count = 1;
            }
        }
        if (i + count > bytes.size()) {
            // Prefix clipping must not invent a replacement glyph for a valid
            // UTF-8 character whose last bytes lie beyond the capture horizon.
            break;
        }

        if (style != previous_style) {
            auto& format = formats[style];
            if (format.isEmpty()) {
                format.setFont(configuration.styles[style].font);
                format.setForeground(configuration.styles[style].foreground);
            }
            QTextLayout::FormatRange range;
            range.start = decoded.text.size();
            range.length = 0;
            range.format = format;
            decoded.formats.append(std::move(range));
            previous_style = style;
        }
        const auto previous_size = decoded.text.size();
        decoded.text.append(QUtf8StringView(utf8 + i, count));
        decoded.formats.last().length += decoded.text.size() - previous_size;
        if (byte_cursors) {
            for (int byte = 0; byte < count; ++byte) {
                (*byte_cursors)[i + byte] = previous_size + (lead < 0x80 ? byte : 0);
            }
        }
        i += count;
    }
    if (byte_cursors) {
        std::fill(byte_cursors->begin() + i, byte_cursors->end(), int(decoded.text.size()));
    }
    return decoded;
}

qreal pixels_per_text_unit(const Configuration& config, const QFontMetricsF& metrics)
{
    const qreal column_width = std::max<qreal>(1, metrics.horizontalAdvance(QLatin1Char('M')));
    return config.pixels.width() / (column_width * config.columns);
}

qreal tab_distance(const Configuration& config, const QFontMetricsF& metrics)
{
    return metrics.horizontalAdvance(QLatin1Char(' ')) * config.tab_width;
}

void configure_layout(QTextLayout& layout, qreal tab_distance)
{
    layout.setCacheEnabled(true);
    QTextOption options;
    options.setWrapMode(QTextOption::NoWrap);
    options.setTextDirection(Qt::LeftToRight);
    options.setTabStopDistance(tab_distance);
    layout.setTextOption(options);
}

QTextLine shape_line(QTextLayout& layout, const Styled_text& decoded, qreal width)
{
    layout.setText(decoded.text);
    layout.setFormats(decoded.formats);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(width);
    layout.endLayout();
    return line;
}

QRgb source_over(QRgb source, QRgb destination)
{
    const int remainder = 255 - qAlpha(source);
    return qRgba(
        qRed(source)   + (qRed(destination)   * remainder + 127) / 255,
        qGreen(source) + (qGreen(destination) * remainder + 127) / 255,
        qBlue(source)  + (qBlue(destination)  * remainder + 127) / 255,
        qAlpha(source) + (qAlpha(destination) * remainder + 127) / 255);
}

}

class Raster_worker::State
{
public:
    explicit State(std::shared_ptr<std::atomic<quint64>> current_generation)
    :
        m_generation(std::move(current_generation))
    {}

    void raster_line(const Line& source, QTextLayout& layout, Style_formats& formats)
    {
        const bool dense = m_configuration.line_advance < 1;
        if (dense) {
            if (const auto* strip = m_strips.object(source.styled_bytes)) {
                composite_strip(*strip, source.number);
                return;
            }
        }
        const Styled_text decoded = decode_line(source.styled_bytes, m_configuration, formats);
        if (decoded.text.isEmpty()) {
            return;
        }

        m_strip.fill(Qt::transparent);
        shape_line(layout, decoded, m_configuration.pixels.width() * k_supersampling / m_scale_x);

        {
            QPainter painter(&m_strip);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.scale(m_scale_x, m_scale_y);
            layout.draw(&painter, QPointF(0, 0));
        }

        // Area-reduce both axes to the available whole samples per line before
        // placing it on the fractional pixel grid. Bilinear drawImage
        // minification samples too few source pixels and produces beats.
        const int strip_height = std::max(1, (int)std::floor(m_configuration.line_advance));
        const QImage strip = m_strip.scaled(
            m_configuration.pixels.width(), strip_height,
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (strip.isNull()) {
            m_allocation_failed = true;
            return;
        }
        composite_strip(strip, source.number);
        if (dense) {
            // Exact styled bytes and this generation's immutable configuration
            // determine the transparent strip. Charge both pixels and retained
            // input, with a minimum cost bounding the number of small entries.
            if (!strip.isNull()) {
                const int cost = (int)std::max<qsizetype>(k_strip_cache_minimum_cost,
                    strip.sizeInBytes() + source.styled_bytes.capacity() + sizeof(QImage) + 128);
                m_strips.insert(source.styled_bytes, new QImage(strip), cost);
            }
        }
    }

    void composite_strip(const QImage& strip, int line_number)
    {
        for (int source_y = 0; source_y < strip.height(); ++source_y) {
            const qreal top = (line_number + (qreal)source_y / strip.height()) *
                m_configuration.line_advance;
            const qreal bottom = std::min<qreal>(m_image.height(),
                (line_number + (qreal)(source_y + 1) / strip.height()) * m_configuration.line_advance);
            const auto* pixels = reinterpret_cast<const QRgb*>(strip.constScanLine(source_y));
            for (int y = (int)std::floor(top); y < (int)std::ceil(bottom); ++y) {
                if (y != m_row_number) {
                    flush_row();
                    m_row_number = y;
                }
                const qreal weight = std::min(bottom, (qreal)y + 1) - std::max(top, (qreal)y);
                for (int x = 0; x < strip.width(); ++x) {
                    auto& coverage = m_row_coverage[x];
                    coverage.red   += qRed(pixels[x])   * weight;
                    coverage.green += qGreen(pixels[x]) * weight;
                    coverage.blue  += qBlue(pixels[x])  * weight;
                    coverage.alpha += qAlpha(pixels[x]) * weight;
                }
            }
        }
    }

    void flush_row()
    {
        if (m_row_number < 0) {
            return;
        }
        // Lines occupy disjoint fractional areas of each output pixel. Sum
        // their coverage before source-over, so packing more lines into a row
        // cannot make an otherwise uniform document periodically brighter.
        auto* destination = reinterpret_cast<QRgb*>(m_image.scanLine(m_row_number));
        for (int x = 0; x < m_image.width(); ++x) {
            const auto& coverage = m_row_coverage[x];
            destination[x] = source_over(qRgba(
                qRound(coverage.red), qRound(coverage.green),
                qRound(coverage.blue), qRound(coverage.alpha)), destination[x]);
        }
        std::fill(m_row_coverage.begin(), m_row_coverage.end(), coverage_t{});
        m_row_number = -1;
    }

    std::shared_ptr<std::atomic<quint64>> m_generation;
    Configuration m_configuration;
    QImage m_image;
    QImage m_strip;
    QCache<QByteArray, QImage> m_strips{k_strip_cache_bytes};
    std::vector<coverage_t> m_row_coverage;
    int m_row_number = -1;
    quint64 m_serial = 0;
    qreal m_scale_x = 1;
    qreal m_scale_y = 1;
    qreal m_tab_distance = 1;
    bool m_allocation_failed = false;
};

Raster_worker::Raster_worker(std::shared_ptr<std::atomic<quint64>> generation)
:
    m_state(std::make_unique<State>(std::move(generation)))
{}

Raster_worker::~Raster_worker() = default;

void Raster_worker::begin(Configuration configuration, quint64 serial)
{
    auto& state = *m_state;
    state.m_strips.clear();
    state.m_configuration = std::move(configuration);
    state.m_serial = serial;
    state.m_image = QImage(state.m_configuration.pixels, QImage::Format_ARGB32_Premultiplied);
    state.m_image.fill(state.m_configuration.background);
    state.m_row_coverage.assign(state.m_image.width(), {});
    state.m_row_number = -1;
    const QFontMetricsF metrics(state.m_configuration.styles[STYLE_DEFAULT].font, &state.m_image);
    state.m_scale_x = k_supersampling * pixels_per_text_unit(state.m_configuration, metrics);
    state.m_scale_y = state.m_scale_x;
    state.m_tab_distance = tab_distance(state.m_configuration, metrics);
    const int strip_height = std::max(1, (int)std::ceil(metrics.height() * state.m_scale_y));
    state.m_strip = QImage(
        state.m_image.width() * k_supersampling, strip_height, QImage::Format_ARGB32_Premultiplied);
    state.m_allocation_failed = state.m_image.isNull() || state.m_strip.isNull();
}

void Raster_worker::consume(Chunk chunk)
{
    auto& state = *m_state;
    bool current = chunk.serial == state.m_serial && !state.m_allocation_failed;
    if (current) {
        // Keep format/font setup and shaping storage for this bounded chunk.
        // setText replaces each line's layout; caching retains that layout
        // through draw instead of discarding it at endLayout and shaping again.
        // Layout and format storage is released before acknowledging the chunk.
        QTextLayout layout(QString(), state.m_configuration.styles[STYLE_DEFAULT].font, &state.m_strip);
        configure_layout(layout, state.m_tab_distance);
        Style_formats formats;
        for (const auto& line : chunk.lines) {
            current = state.m_generation->load(std::memory_order_relaxed) == chunk.serial;
            if (!current) {
                break;
            }
            state.raster_line(line, layout, formats);
        }
    }
    const bool final = (current && chunk.final) || state.m_allocation_failed;
    if (current && final && !state.m_allocation_failed) {
        state.flush_row();
    }
    emit chunk_finished(chunk.serial, final, final ? state.m_image : QImage(), state.m_allocation_failed);
    if (!current || final) {
        clear();
    }
}

QVector<QRect> Raster_worker::occurrence_marks(
    const Configuration& config, const QVector<Occurrence_line>& lines, int edge_width)
{
    const int width = config.pixels.width();
    // Use the same image-device metrics as the source raster after its working
    // strip has been released. Dimensions do not affect its font metrics.
    QImage device(1, 1, QImage::Format_ARGB32_Premultiplied);
    QTextLayout layout(QString(), config.styles[STYLE_DEFAULT].font, &device);
    const QFontMetricsF metrics(config.styles[STYLE_DEFAULT].font, &device);
    const qreal scale = pixels_per_text_unit(config, metrics);
    configure_layout(layout, tab_distance(config, metrics));
    Style_formats formats;
    QVector<int> cursors;
    QVector<QRect> marks;
    for (const auto& entry : lines) {
        const int row = std::min(config.pixels.height() - 1, int(entry.source.number * config.line_advance));
        const auto decoded = decode_line(entry.source.styled_bytes, config, formats, &cursors);
        const auto line = decoded.text.isEmpty() ? QTextLine() : shape_line(layout, decoded, width / scale);
        bool edge_mark = false;
        for (const auto& [start, end] : entry.ranges) {
            if (!line.isValid() || start >= cursors.size() - 1 || cursors[start] == cursors.back()) {
                edge_mark = true;
                continue;
            }
            const int first = cursors[start];
            const int after = cursors[std::min(end, int(cursors.size() - 1))];
            // At a bidi boundary a logical caret can belong to the adjacent
            // run. The selected glyph runs carry their actual visual bounds.
            const auto runs = line.glyphRuns(first, after - first);
            bool visible = false;
            for (const auto& run : runs) {
                const auto bounds = run.boundingRect();
                const qreal left = bounds.left() * scale;
                if (left >= width || bounds.right() < 0) {
                    continue;
                }
                const int x = std::clamp(int(std::floor(left)), 0, width - 1);
                const int limit = std::clamp(int(std::ceil(bounds.right() * scale)), x + 1, width);
                marks.append(QRect(x, row, limit - x, 1));
                visible = true;
            }
            if (!visible) {
                edge_mark = true;
            }
        }
        if (edge_mark) {
            marks.append(QRect(width - edge_width, row, edge_width, 1));
        }
    }
    return marks;
}

void Raster_worker::clear()
{
    m_state->m_strips.clear();
    m_state->m_row_coverage.clear();
    m_state->m_row_number = -1;
    m_state->m_image = {};
    m_state->m_strip = {};
}

}
