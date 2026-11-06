#pragma once
// -----------------------------------------------------------------------------
// meshoptimizer stage: turns raw contour output into GPU- and physics-ready data.
//
//   1. optimizeVertexCache   - post-transform cache locality (index reorder)
//   2. optimizeOverdraw      - cluster reorder for early-Z (threshold 1.05)
//   3. optimizeVertexFetch   - vertex reorder + drops unreferenced DC vertices
//   4. buildMeshlets         - 64 verts / 124 tris clusters for task/mesh shaders
//      computeMeshletBounds  - sphere + normal cone for amplification culling
//      optimizeMeshlet       - per-cluster locality
//   5. simplify (LockBorder) - collision mesh at ~50% triangles. LockBorder
//                              pins the open chunk boundary so neighbouring
//                              collision meshes stay sealed (no tunnelling).
//
// Thread safety: pure function of its inputs.
// -----------------------------------------------------------------------------

#include "vxe/meshing/ChunkMesh.h"

namespace vxe {

struct MeshOptSettings {
    uint32_t meshletMaxVertices = 64;
    uint32_t meshletMaxTriangles = 124;     // 124 keeps the index payload 4-byte aligned
    float meshletConeWeight = 0.25f;
    float overdrawThreshold = 1.05f;
    bool buildMeshlets = true;
    bool buildCollision = true;
    float collisionTargetRatio = 0.5f;      // fraction of triangles kept
    float collisionMaxErrorMetres = 0.05f;  // absolute error bound
};

void optimizeChunkMesh(ChunkMesh&& in, const MeshOptSettings& settings, OptimizedChunkMesh& out);

} // namespace vxe
