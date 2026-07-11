#pragma once

#include <vector>

#include "Helpers/XoaCoordinates.h"

//==============================================================================
// XOA - 3-D convex hull seam (WP7). The one place the vendored
// ThirdParty/convhull_3d quickhull is exposed to the codebase; the single
// CONVHULL_3D_ENABLE implementation TU lives in ConvexHull.cpp.
//
// Non-RT (allocates): used by the AllRAD/VBAP design path on the control or
// rebuild-worker thread, never on the audio thread.
//==============================================================================

namespace xoa::hull
{

/** Triangle faces of the 3-D convex hull of the given points.

    Returns flat [i0, i1, i2] index triples into the input array (size is a
    multiple of 3). Empty on hard failure (fewer than 4 points, or the
    quickhull could not build anything). NOTE: a degenerate coplanar set may
    still return faces - flat sliver triangles - rather than failing; the
    consumer must reject those itself (AmbiVBAP's determinant guard does, and
    then inserts imaginary speakers and retries). */
std::vector<int> triangleFaces (const coords::Cartesian* points, int count);

} // namespace xoa::hull
