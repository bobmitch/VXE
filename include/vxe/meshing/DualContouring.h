#pragma once
// -----------------------------------------------------------------------------
// Dual Contouring / Surface Nets isosurface extraction on a uniform padded grid.
//
// One vertex per sign-changing cell, one quad per sign-changing edge. The
// vertex is placed by minimising a QEF built from Hermite data (edge
// intersection points + density gradients), which preserves sharp creases
// (cliff edges, carved cuts, building-tool flattening). `sharpness` blends
// between the QEF solution (1.0, Dual Contouring) and the Hermite mass point
// (0.0, Surface Nets) - Surface Nets is preferable for soft organic LODs.
//
// Seams: the mesher visits cells [0, N] (one more than the chunk owns) and
// emits quads only for edges the chunk *owns*:
//     along-axis coordinate in [0, N-1], perpendicular coordinates in [1, N].
// Each global edge is therefore emitted exactly once, and the shared cell
// vertices at the boundary are computed from identical samples in both
// chunks -> watertight joins without stitching passes.
//
// LOD transitions (different strides) have no shared dual structure: where a
// stride-s chunk meets a stride-2s one, four fine cells face one coarse cell,
// so there is no edge both sides agree to emit a quad for and the two surfaces
// simply end near each other. `skirtDepthCells` covers that gap with a collar
// (see appendBorderSkirt); proper transition cells are still the long-term fix,
// see README "Known limitations".
//
// Thread safety: free function, scratch is thread_local. Safe to run any
// number of instances concurrently on distinct inputs.
// -----------------------------------------------------------------------------

#include "vxe/meshing/ChunkMesh.h"

namespace vxe {

struct MesherSettings {
    float sharpness = 1.0f;            // 1 = Dual Contouring, 0 = Surface Nets
    float svdRelativeThreshold = 0.1f; // eigenvalues below max*thr are truncated (feature detection)
    float maxQefErrorCells = 0.25f;    // fall back to mass point if residual exceeds this (in cell^2)
    // Border skirt depth, in cells; 0 disables it. The collar hangs off the
    // chunk perimeter along -normal. At a same-LOD seam it is buried under the
    // neighbour's surface and never drawn; at an inter-LOD seam it plugs the
    // crack. Both sides carry one, so it covers the neighbour's surface landing
    // either above or below this one.
    float skirtDepthCells = 0.f;
};

struct MesherStats {
    uint32_t activeCells = 0;
    uint32_t sharpVertices = 0;        // vertices whose QEF had rank >= 2 (edges/corners)
    uint32_t openEdges = 0;            // mesh edges used by one triangle, i.e. the chunk perimeter
    uint32_t skirtTriangles = 0;
};

// Extracts the isosurface of `field` for a chunk sampled at `voxelStep` metres.
MesherStats contourChunk(const DensityField& field, float voxelStep, const MesherSettings& settings, ChunkMesh& out);

} // namespace vxe
