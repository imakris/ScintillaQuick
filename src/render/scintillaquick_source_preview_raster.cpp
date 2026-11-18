// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_source_preview_raster_p.h"

#include <Scintilla.h>

#include <QCache>
#include <QFontMetricsF>
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

Styled_text decode_line(const QByteArray& pairs, const Configuration& configuration, Style_formats& formats)
{
    QByteArray bytes(pairs.size() / 2, Qt::Uninitialized);
    auto* utf8 = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        utf8[i] = pairs[i * 2];
    }

    Styled_text decoded;
    decoded.text.reserve(bytes.size());
    int previous_style = -1;
    for (qsizetype i = 0; i < bytes.size();) {
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
        i += count;
    }
    return decoded;
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
        layout.setText(decoded.text);
        layout.setFormats(decoded.formats);
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(m_configuration.pixels.width() * k_supersampling / m_scale_x);
        layout.endLayout();

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
    const qreal column_width = std::max<qreal>(1, metrics.horizontalAdvance(QLatin1Char('M')));
    state.m_scale_x = state.m_image.width() * k_supersampling / (column_width * state.m_configuration.columns);
    state.m_scale_y = state.m_scale_x;
    state.m_tab_distance = metrics.horizontalAdvance(QLatin1Char(' ')) * state.m_configuration.tab_width;
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
        layout.setCacheEnabled(true);
        QTextOption options;
        options.setWrapMode(QTextOption::NoWrap);
        options.setTextDirection(Qt::LeftToRight);
        options.setTabStopDistance(state.m_tab_distance);
        layout.setTextOption(options);
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

void Raster_worker::clear()
{
    m_state->m_strips.clear();
    m_state->m_row_coverage.clear();
    m_state->m_row_number = -1;
    m_state->m_image = {};
    m_state->m_strip = {};
}

}
