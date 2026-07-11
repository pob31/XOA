/*
    ConvexHull.cpp - the single translation unit that compiles the vendored
    convhull_3d implementation (see ThirdParty/convhull_3d/, MIT). Everything
    else includes only the ConvexHull.h seam.
*/

#include "DSP/ConvexHull.h"

#include <cstdlib>

#if defined(_MSC_VER)
 #pragma warning(push)
 #pragma warning(disable: 4100 4201 4244 4245 4267 4305 4701 4702 4703)
#endif

#define CONVHULL_3D_ENABLE
#include "../../ThirdParty/convhull_3d/convhull_3d.h"

#if defined(_MSC_VER)
 #pragma warning(pop)
#endif

namespace xoa::hull
{

std::vector<int> triangleFaces (const coords::Cartesian* points, int count)
{
    if (points == nullptr || count < 4)
        return {};

    std::vector<ch_vertex> vertices ((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        vertices[(size_t) i].x = points[i].x;
        vertices[(size_t) i].y = points[i].y;
        vertices[(size_t) i].z = points[i].z;
    }

    int* faces = nullptr;
    int numFaces = 0;
    convhull_3d_build (vertices.data(), count, &faces, &numFaces);

    if (faces == nullptr || numFaces <= 0)
    {
        std::free (faces);
        return {};
    }

    std::vector<int> out (faces, faces + (size_t) numFaces * 3);
    std::free (faces);   // convhull_3d allocates with malloc
    return out;
}

} // namespace xoa::hull
