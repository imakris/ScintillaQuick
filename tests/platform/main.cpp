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
    for (const QString& body : {QString(),
                               QStringLiteral("plain ASCII"),
                               QStringLiteral("e\u0301 \u03b1"),
                               marker + QStringLiteral("body"),
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
        const std::string_view text(bytes.constData(), bytes.size());
        QTextLayout layout(body, qt_font, &image);
        layout.beginLayout();
        const QTextLine line = layout.createLine();
        layout.endLayout();
        for (const auto measure_widths : {
                &Scintilla::Internal::Surface_impl::MeasureWidthsUTF8,
                &Scintilla::Internal::Surface_impl::MeasureWidths})
        {
            (surface.*measure_widths)(font.get(), text, positions.data());
            size_t byte_index = 0;
            for (qsizetype character = 0; character < body.size(); ++character) {
                const qreal expected = line.cursorToX(static_cast<int>(character + 1));
                const QByteArray character_bytes = body.mid(character, 1).toUtf8();
                for (qsizetype byte = 0; byte < character_bytes.size(); ++byte) {
                    SQ_EXPECT(std::abs(positions[byte_index++] - expected) < 0.01);
                }
            }
        }
        const qreal expected_width = QFontMetricsF(qt_font, &image).horizontalAdvance(body);
        SQ_EXPECT(std::abs(surface.WidthText(font.get(), text) - expected_width) < 0.01);
        SQ_EXPECT(std::abs(surface.WidthTextUTF8(font.get(), text) - expected_width) < 0.01);
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
                    for (const auto measure_widths : {
                            &Surface_impl::MeasureWidthsUTF8,
                            &Surface_impl::MeasureWidths})
                    {
                        (surface.*measure_widths)(font.get(), text, positions.data());
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
                        SQ_EXPECT(std::abs(surface.WidthTextUTF8(font.get(), text) - expected_width) < 0.01);
                    }
                }
            }
        }
    }
}

void test_font_stretch_matches_qt()
{
    using namespace Scintilla::Internal;
    for (const Scintilla::FontStretch stretch : {
            Scintilla::FontStretch::Condensed,
            Scintilla::FontStretch::Normal,
            Scintilla::FontStretch::Expanded})
    {
        FontParameters parameters("Cousine", 11);
        parameters.stretch = stretch;
        const auto font = Font::Allocate(parameters);
        QFont expected(QStringLiteral("Cousine"));
        expected.setPointSizeF(11);
        expected.setStretch(stretch == Scintilla::FontStretch::Condensed ? QFont::Condensed :
            stretch == Scintilla::FontStretch::Expanded ? QFont::Expanded : QFont::Unstretched);
        SQ_EXPECT(realized_font(font.get()).stretch() == expected.stretch());
    }
}

void test_surface_measurement_preserves_word_spacing()
{
    using namespace Scintilla::Internal;
    for (const QString& family : {QStringLiteral("Cascadia Code"), QStringLiteral("Cousine")}) {
        QFont qt_font(family, 11);
        qt_font.setWordSpacing(4);
        const auto font = font_from_qfont(qt_font);
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
        Surface_impl surface;
        surface.Init(static_cast<SurfaceID>(&image), nullptr);

        for (const auto measure_widths : {
                &Surface_impl::MeasureWidthsUTF8,
                &Surface_impl::MeasureWidths})
        {
            // Warm the cache with text that has no spaces before measuring
            // text whose space advances include the font's word spacing.
            for (const std::string_view text : {"abcdefgh", "a a", " != "}) {
                std::vector<XYPOSITION> positions(text.size());
                (surface.*measure_widths)(font.get(), text, positions.data());
                const QString qt_text = QString::fromLatin1(text.data(), text.size());
                QTextLayout layout(qt_text, qt_font, &image);
                layout.beginLayout();
                const QTextLine line = layout.createLine();
                layout.endLayout();
                for (size_t i = 0; i < text.size(); ++i) {
                    SQ_EXPECT(std::abs(positions[i] -
                        line.cursorToX(static_cast<int>(i + 1))) < 0.01);
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
    test_surface_measurement_preserves_word_spacing();
    test_font_stretch_matches_qt();
    return g_failures == 0 ? 0 : 1;
}
