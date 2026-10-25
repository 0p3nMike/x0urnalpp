#include "PlainBackgroundView.h"

#include <algorithm>  // for max, min

#include "util/safe_casts.h"                 // for ceil_cast, floor_cast
#include "view/background/BackgroundView.h"  // for BackgroundView, view

using namespace xoj::view;

PlainBackgroundView::PlainBackgroundView(double pageWidth, double pageHeight, Color backgroundColor):
        BackgroundView(pageWidth, pageHeight), backgroundColor(backgroundColor) {}

void PlainBackgroundView::draw(cairo_t* cr) const {
    Util::cairo_set_source_rgbi(cr, this->backgroundColor);
    cairo_paint(cr);
}

std::pair<int, int> PlainBackgroundView::getIndexBounds(double min, double max, double step, double margin,
                                                        double length) {
    return {ceil_cast<int>(std::max(min, margin) / step), floor_cast<int>(std::min(max, length - margin) / step)};
}

/*
 * X0urnal++: the same thing without the page-edge clamps.
 *
 * The bounded version pins the grid inside [margin, length - margin], which is
 * right for a sheet of paper and wrong for a whiteboard: it stops the ruling at
 * the old page edge and, because `margin` is never below half a line width, it
 * also prevents anything being drawn at a negative coordinate. Here the range
 * comes purely from the clip, so the paper covers whatever is on screen.
 */
std::pair<int, int> PlainBackgroundView::getIndexBoundsUnbounded(double min, double max, double step) {
    return {ceil_cast<int>(min / step), floor_cast<int>(max / step)};
}
