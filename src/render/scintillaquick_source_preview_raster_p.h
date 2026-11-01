// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>

#include <array>
#include <atomic>
#include <memory>

namespace ScintillaQuick_preview {

constexpr int k_style_count = 256;
constexpr int k_prefix_bytes = 1024;
constexpr int k_chunk_lines = 128;

struct Style
{
    QFont font;
    QColor foreground;
};

struct Configuration
{
    std::array<Style, k_style_count> styles;
    QSize pixels;
    QColor background;
    qreal line_advance = 1;
    int line_count = 1;
    int columns = 120;
    int tab_width = 4;
};

struct Line
{
    // Scintilla's interleaved UTF-8 byte/style pairs, clipped before copying.
    QByteArray styled_bytes;
    int number = 0;
};

struct Chunk
{
    QVector<Line> lines;
    quint64 serial = 0;
    bool final = false;
};

class Raster_worker : public QObject
{
    Q_OBJECT

public:
    explicit Raster_worker(std::shared_ptr<std::atomic<quint64>> generation);
    ~Raster_worker() override;

    void begin(Configuration configuration, quint64 serial);
    void consume(Chunk chunk);
    void clear();

signals:
    void chunk_finished(
        quint64 serial, bool final, const QImage& image,
        bool allocation_failed);

private:
    class State;
    std::unique_ptr<State> m_state;
};

}
