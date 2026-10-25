/*
 * X0urnal++ -- which part of a page is kept in the render buffer.
 * See ViewportBuffer.h for the why.
 *
 * @license GNU GPLv2 or later
 */

#include "ViewportBuffer.h"

#include <algorithm>
#include <cmath>

#include "model/XojPage.h"

using namespace xoj::view;

namespace {

/// The whole page: what every ordinary document gets, and what upstream always used.
Range wholePage(const XojPage& page) { return Range(0.0, 0.0, page.getWidth(), page.getHeight()); }

/**
 * Snap a range outwards to a grid of `quantumPageUnits`.
 *
 * Outwards on both sides, never inwards: rounding a resident region inwards
 * could leave a sliver of the viewport unrendered, which is the one thing this
 * must never do.
 */
Range snapOutwards(const Range& r, double quantumPageUnits) {
    if (quantumPageUnits <= 0.0) {
        return r;
    }
    return Range(std::floor(r.minX / quantumPageUnits) * quantumPageUnits,
                 std::floor(r.minY / quantumPageUnits) * quantumPageUnits,
                 std::ceil(r.maxX / quantumPageUnits) * quantumPageUnits,
                 std::ceil(r.maxY / quantumPageUnits) * quantumPageUnits);
}

}  // namespace

Range ViewportBuffer::residentRange(const XojPage& page, const Range& visible, double zoom, int dpiScale) {
    const Range whole = wholePage(page);

    /*
     * An ordinary page renders exactly as it always did. This early return is
     * the single most important line in the file: paged and PDF documents must
     * be bit-identical to before, and they are, because nothing downstream can
     * tell this apart from upstream's hardcoded Range(0, 0, w, h).
     */
    if (!page.isInfiniteCanvas()) {
        return whole;
    }

    /*
     * A page that is not on screen keeps the whole page. It is about to be
     * discarded or is small anyway, and computing a viewport for it would mean
     * inventing one.
     */
    if (!visible.isValid() || visible.empty()) {
        return whole;
    }

    if (zoom <= 0.0) {
        zoom = 1.0;
    }
    if (dpiScale < 1) {
        dpiScale = 1;
    }

    /* Keep a margin around the viewport so ordinary scrolling never leaves the
       rendered region -- the cheap version of a prefetch ring. */
    Range resident = visible;
    const double padX = visible.getWidth() * PADDING_FRACTION;
    const double padY = visible.getHeight() * PADDING_FRACTION;
    resident = Range(visible.minX - padX, visible.minY - padY, visible.maxX + padX, visible.maxY + padY);

    /* Snap to a device-pixel grid so small scrolls reuse the same surface
       instead of reallocating on every frame. The quantum is expressed in
       DEVICE pixels, so it must be converted through zoom and the HiDPI factor;
       doing this in page units instead would make the grid coarser or finer
       with zoom and defeat the purpose. */
    const double devicePerPageUnit = zoom * static_cast<double>(dpiScale);
    resident = snapOutwards(resident, static_cast<double>(QUANTUM_DEVICE_PX) / devicePerPageUnit);

    /* Never resident outside the page itself. */
    resident = resident.intersect(whole);
    if (!resident.isValid() || resident.empty()) {
        return whole;
    }

    /*
     * Safety net against cairo's 32767 px per-side limit. Upstream never checks
     * cairo_surface_status, so an oversized request becomes a nil surface and a
     * blank page rather than an error. The policy above should never get close,
     * but a pathological zoom must degrade to a smaller region, not break.
     */
    const double maxSidePageUnits = static_cast<double>(MAX_DEVICE_PX_PER_SIDE) / devicePerPageUnit;
    if (resident.getWidth() > maxSidePageUnits || resident.getHeight() > maxSidePageUnits) {
        const double cx = 0.5 * (resident.minX + resident.maxX);
        const double cy = 0.5 * (resident.minY + resident.maxY);
        const double halfW = 0.5 * std::min(resident.getWidth(), maxSidePageUnits);
        const double halfH = 0.5 * std::min(resident.getHeight(), maxSidePageUnits);
        resident = Range(cx - halfW, cy - halfH, cx + halfW, cy + halfH).intersect(whole);
    }

    return resident;
}

bool ViewportBuffer::needsRealloc(const Range& resident, const Range& visible) {
    if (!visible.isValid() || visible.empty()) {
        return false;  // off screen: keep what we have rather than thrash
    }
    if (!resident.isValid() || resident.empty()) {
        return true;  // nothing resident yet
    }
    /* Reallocate only once the viewport actually leaves the resident region.
       Range::contains takes a Rectangle, so the comparison is done on the
       bounds directly. */
    return visible.minX < resident.minX || visible.minY < resident.minY || visible.maxX > resident.maxX ||
           visible.maxY > resident.maxY;
}
