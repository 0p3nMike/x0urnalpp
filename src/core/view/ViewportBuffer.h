/*
 * X0urnal++ -- which part of a page is kept in the render buffer.
 *
 * THE PROBLEM THIS SOLVES
 *
 * RenderJob allocates one cairo surface covering the WHOLE page at the current
 * zoom, times the HiDPI scale factor. Measured with a probe build:
 * an A4 page costs 1.9 MB, a 150x150 cm page costs 69.0 MB, and process private
 * memory rose 39.4 -> 107.9 MB between the two with an identical window size.
 * The growth is quadratic in page size, so a canvas big enough to be useful as a
 * whiteboard is unusable, and cairo refuses image surfaces beyond 32767 px per
 * side -- on a HiDPI screen at 7x zoom that ceiling is a page of only ~0.83 m.
 *
 * So for a page marked as an infinite canvas, only the visible region plus a
 * margin is ever rasterised. Everything else in the pipeline already supports
 * this and simply was never asked to:
 *
 *   - Mask stores xOffset/yOffset and paintTo() positions itself, so a buffer
 *     covering part of a page blits into the right place unaided.
 *   - LayerView::draw culls elements against the clip, so drawing costs what is
 *     visible rather than what exists.
 *   - The ruling backgrounds derive their grid indices from cairo_clip_extents,
 *     so they tile over whatever region they are given.
 *   - RenderJob::rerenderRectangle already renders arbitrary sub-rectangles.
 *
 * WHY THE POLICY LIVES IN ITS OWN CLASS
 *
 * Keeping the decision here means the change inside upstream's RenderJob is a
 * single call, and the eventual upgrade to a real tile cache (fixed tiles, LRU,
 * prefetch -- what Figma and map renderers do) replaces this file rather than
 * re-opening the render path.
 *
 * THE MOST IMPORTANT PROPERTY: for an ordinary page this returns the full page
 * rect, so paged and PDF documents render exactly as they did before. Notebook
 * mode is opt-in per page and everything else is untouched.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include "util/Range.h"

class XojPage;

namespace xoj::view {

class ViewportBuffer {
public:
    /**
     * The region of a page that should be resident in its render buffer.
     *
     * Returns the whole page when the page is not an infinite canvas, which is
     * what makes this a no-op for ordinary documents.
     *
     * @param page      the page being rendered
     * @param visible   the visible part of that page, in page coordinates
     * @param zoom      current zoom
     * @param dpiScale  gtk_widget_get_scale_factor(); multiplies surface pixels
     */
    static Range residentRange(const XojPage& page, const Range& visible, double zoom, int dpiScale);

    /**
     * Whether the resident region no longer covers what is visible, i.e. the
     * buffer must be reallocated around the new viewport.
     *
     * False when `visible` is empty: a page scrolled off screen keeps whatever
     * it has rather than thrashing.
     */
    static bool needsRealloc(const Range& resident, const Range& visible);

    /**
     * How much margin is kept around the viewport, as a fraction of the
     * viewport's own size. This is what makes ordinary scrolling free: the
     * pointer moves inside an already-rendered region and nothing re-renders.
     *
     * Bigger is smoother but costs memory quadratically; 0.5 keeps roughly 4x
     * the viewport resident, which at 1600x1200 logical pixels is ~29 MB at
     * dpiScale 2 -- the cost of a couple of A4 pages, and constant no matter how
     * large the canvas grows.
     */
    static constexpr double PADDING_FRACTION = 0.5;

    /**
     * The resident region is snapped outwards to a multiple of this many DEVICE
     * pixels. Without it every scrolled pixel would produce a slightly different
     * rectangle and reallocate the surface continuously; with it the buffer is
     * stable across small movements and is reused.
     */
    static constexpr int QUANTUM_DEVICE_PX = 256;

    /**
     * Cap on the resident area, in device pixels, as a safety net.
     *
     * cairo refuses image surfaces over 32767 px per side and NOTHING in
     * upstream checks cairo_surface_status, so exceeding it yields a nil surface
     * and a blank page rather than an error. The viewport policy should never
     * approach this, but a pathological zoom must degrade rather than break.
     */
    static constexpr int MAX_DEVICE_PX_PER_SIDE = 16384;
};

}  // namespace xoj::view
