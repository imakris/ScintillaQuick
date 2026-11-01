// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_source_preview_raster_p.h"

#include <Scintilla.h>

#include <QFontMetricsF>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ScintillaQuick_preview {
namespace {

constexpr int k_supersampling = 4;
struct Styled_text
{
    QString text;
    QList<QTextLayout::FormatRange> formats;
};

Styled_text decode_line(const QByteArray& pairs, const Configuration& configuration)
{
    QByteArray bytes(pairs.size() / 2, Qt::Uninitialized);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        bytes[i] = pairs[i * 2];
    }

    Styled_text decoded;
    int previous_style = -1;
    for (qsizetype i = 0; i < bytes.size();) {
        const auto lead = (unsigned char)bytes[i];
        if (lead == '\r' || lead == '\n') {
            break;
        }

        const int style = (unsigned char)pairs[i * 2 + 1];
        int count = 1;
        if (lead < 0x80) {
            // Keep ASCII tabs/controls intact while avoiding a temporary
            // QString for each byte. Non-ASCII decoding below is unchanged.
            while (i + count < bytes.size()) {
                const auto next = (unsigned char)bytes[i + count];
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
                valid_tail &= ((unsigned char)bytes[i + tail] & 0xc0) == 0x80;
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

        const QString fragment = QString::fromUtf8(bytes.constData() + i, count);
        if (style != previous_style) {
            QTextLayout::FormatRange range;
            range.start = decoded.text.size();
            range.length = 0;
            range.format.setFont(configuration.styles[style].font);
            range.format.setForeground(configuration.styles[style].foreground);
            decoded.formats.append(std::move(range));
            previous_style = style;
        }
        decoded.text += fragment;
        decoded.formats.last().length += fragment.size();
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

    void raster_line(const Line& source)
    {
        const Styled_text decoded = decode_line(source.styled_bytes, m_configuration);
        if (decoded.text.isEmpty()) {
            return;
        }

        m_strip.fill(Qt::transparent);
        QTextLayout layout(decoded.text, m_configuration.styles[STYLE_DEFAULT].font, &m_strip);
        QTextOption options;
        options.setWrapMode(QTextOption::NoWrap);
        options.setTextDirection(Qt::LeftToRight);
        options.setTabStopDistance(m_tab_distance);
        layout.setTextOption(options);
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

        const qreal y = source.number * m_configuration.line_advance;
        if (m_configuration.line_advance < 1) {
            // Every line contributes actual glyph coverage to its physical
            // row. Source-over accumulation retains tiny isolated blocks among
            // blank lines instead of sampling one line from the bucket.
            const QImage row = m_strip.scaled(
                m_configuration.pixels.width(), 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            auto* destination = reinterpret_cast<QRgb*>(m_image.scanLine((int)y));
            const auto* pixels = reinterpret_cast<const QRgb*>(row.constScanLine(0));
            for (int x = 0; x < row.width(); ++x) {
                destination[x] = source_over(pixels[x], destination[x]);
            }
        }
        else {
            QPainter painter(&m_image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(
                QRectF(0, y, m_image.width(), m_configuration.line_advance), m_strip);
        }
    }

    std::shared_ptr<std::atomic<quint64>> m_generation;
    Configuration m_configuration;
    QImage m_image;
    QImage m_strip;
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
    state.m_configuration = std::move(configuration);
    state.m_serial = serial;
    state.m_image = QImage(state.m_configuration.pixels, QImage::Format_ARGB32_Premultiplied);
    state.m_image.fill(state.m_configuration.background);
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
    for (const auto& line : chunk.lines) {
        current = current && state.m_generation->load(std::memory_order_relaxed) == chunk.serial;
        if (!current) {
            break;
        }
        state.raster_line(line);
    }
    const bool final = (current && chunk.final) || state.m_allocation_failed;
    emit chunk_finished(chunk.serial, final, final ? state.m_image : QImage(), state.m_allocation_failed);
    if (!current || final) {
        clear();
    }
}

void Raster_worker::clear()
{
    m_state->m_image = {};
    m_state->m_strip = {};
}

}
