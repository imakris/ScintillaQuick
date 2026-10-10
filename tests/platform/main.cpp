// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_platqt.h"
#include "scintillaquick_font.h"
#include "scintillaquick_test_macros.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QTextLayout>

#include <cmath>
#include <string_view>
#include <vector>

namespace
{

int g_failures = 0;

void test_utf8_body_prefix_measurement()
{
    const QString marker(QChar(0xFEFF));
    for (const QString& body : {marker + QStringLiteral("body"),
                               marker + marker + QStringLiteral("body"),
                               QStringLiteral("prefix") + marker + QStringLiteral("body")})
    {
        const QByteArray bytes = body.toUtf8();
        const QByteArray family = QByteArrayLiteral("Cousine");
        const auto font = Scintilla::Internal::Font::Allocate(
            Scintilla::Internal::FontParameters(family.constData(), 11));
        const QFont qt_font(QStringLiteral("Cousine"), 11);
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
        Scintilla::Internal::Surface_impl surface;
        surface.Init(static_cast<Scintilla::Internal::SurfaceID>(&image), nullptr);
        std::vector<Scintilla::Internal::XYPOSITION> positions(static_cast<size_t>(bytes.size()));
        surface.MeasureWidths(font.get(), std::string_view(bytes.constData(), bytes.size()), positions.data());
        QTextLayout layout(body, qt_font, &image);
        layout.beginLayout();
        const QTextLine line = layout.createLine();
        layout.endLayout();
        size_t byte_index = 0;
        for (qsizetype character = 0; character < body.size(); ++character) {
            const qreal expected = line.cursorToX(static_cast<int>(character + 1));
            const QByteArray character_bytes = body.mid(character, 1).toUtf8();
            for (qsizetype byte = 0; byte < character_bytes.size(); ++byte) {
                SQ_EXPECT(std::abs(positions[byte_index++] - expected) < 0.01);
            }
        }
    }
}

void test_surface_measurement_tracks_device_dpi()
{
    using namespace Scintilla::Internal;
    for (const QString& family : {QStringLiteral("Cascadia Code"), QStringLiteral("Cousine")}) {
        const QByteArray family_utf8 = family.toUtf8();
        const auto font = Font::Allocate(FontParameters(family_utf8.constData(), 11));
        QFont qt_font(family);
        qt_font.setPointSizeF(11);
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
        Surface_impl surface;
        surface.Init(static_cast<SurfaceID>(&image), nullptr);
        constexpr std::string_view text = "abcdefgh";
        const QString qt_text = QString::fromLatin1(text.data(), text.size());
        std::vector<XYPOSITION> positions(text.size());

        // Reuse the font and device through DPI changes; repeat each measurement
        // so both initial shaping and the populated advance cache are checked.
        for (const qreal device_pixel_ratio : {1.0, 1.25}) {
            image.setDevicePixelRatio(device_pixel_ratio);
            for (const int dots_per_meter : {3780, 5669, 3780}) {
                image.setDotsPerMeterX(dots_per_meter);
                image.setDotsPerMeterY(dots_per_meter);
                QTextLayout layout(qt_text, qt_font, &image);
                layout.beginLayout();
                const QTextLine line = layout.createLine();
                layout.endLayout();
                for (int repeat = 0; repeat < 2; ++repeat) {
                    surface.MeasureWidths(font.get(), text, positions.data());
                    for (size_t i = 0; i < text.size(); ++i) {
                        const qreal expected = line.cursorToX(static_cast<int>(i + 1));
                        if (std::abs(positions[i] - expected) >= 0.01) {
                            std::fprintf(stderr,
                                "font=%s dpm=%d dpr=%.2f repeat=%d byte=%zu actual=%.9f expected=%.9f\n",
                                family_utf8.constData(), dots_per_meter, device_pixel_ratio,
                                repeat, i, positions[i], expected);
                        }
                        SQ_EXPECT(std::abs(positions[i] - expected) < 0.01);
                    }
                    const qreal expected_width = QFontMetricsF(qt_font, &image).horizontalAdvance(qt_text);
                    SQ_EXPECT(std::abs(positions.back() - expected_width) < 0.01);
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qFatal("%s", qPrintable(font_error));
    }
    test_utf8_body_prefix_measurement();
    test_surface_measurement_tracks_device_dpi();
    return g_failures == 0 ? 0 : 1;
}
