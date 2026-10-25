/*
 * Xournal++
 *
 * Base class for isometric backgrounds
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
class BaseIsometricBackgroundView: public OneColorBackgroundView {
public:
    BaseIsometricBackgroundView(double pageWidth, double pageHeight, Color backgroundColor,
                                const BackgroundConfig& config, double defaultLineWidth);
    virtual ~BaseIsometricBackgroundView() = default;

    virtual void draw(cairo_t* cr) const override;

protected:
    virtual void paintGrid(cairo_t* cr, int cols, int rows, double xstep, double ystep, double xOffset,
                           double yOffset) const = 0;

protected:
    double triangleSize = 14.17;  // 5mm

    constexpr static Color DEFAULT_LINE_COLOR = Colors::xopp_silver;
    constexpr static Color ALT_DEFAULT_LINE_COLOR = Color{0xff2d4f7aU};  // X0urnal++: blackboard
    // grid -- a blue rule on the void ground. Measured 4.48:1 against #05070a: clearly
    // blue and legible, while staying well under the ink so the paper never competes
    // with what is written on it (full cobalt #4f8ad6 is 7.35:1 and does compete).
};
};  // namespace xoj::view
