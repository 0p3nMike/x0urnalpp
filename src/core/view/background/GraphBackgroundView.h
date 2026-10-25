/*
 * Xournal++
 *
 * Class for graph backgrounds
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cairo.h>  // for cairo_t

#include "util/Color.h"  // for Color

#include "OneColorBackgroundView.h"  // for OneColorBackgroundView

class BackgroundConfig;

namespace xoj::view {
class GraphBackgroundView: public OneColorBackgroundView {
public:
    GraphBackgroundView(double pageWidth, double pageHeight, Color backgroundColor, const BackgroundConfig& config);
    virtual ~GraphBackgroundView() = default;

    virtual void draw(cairo_t* cr) const override;

protected:
    bool roundUpMargin = false;
    double margin = 0.0;
    double squareSize = 14.17;  // 5mm
    int boldLineInterval = 0;   // 0 means no bold lines, otherwise every Nth line is bold
    double boldLineWidth = 1.5;

    constexpr static Color DEFAULT_LINE_COLOR = Colors::xopp_silver;
    constexpr static Color ALT_DEFAULT_LINE_COLOR = Color{0xff2d4f7aU};  // X0urnal++: blackboard
    // grid -- a blue rule on the void ground. Measured 4.48:1 against #05070a: clearly
    // blue and legible, while staying well under the ink so the paper never competes
    // with what is written on it (full cobalt #4f8ad6 is 7.35:1 and does compete).
    constexpr static double DEFAULT_LINE_WIDTH = 0.5;
    constexpr static double DEFAULT_BOLD_LINE_WIDTH = 1.5;
};
};  // namespace xoj::view
