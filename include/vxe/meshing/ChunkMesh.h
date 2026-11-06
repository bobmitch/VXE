#pragma once
#include "vxe/core/Math.h"
#include "vxe/world/DensityField.h"

#include <cstdint>
#include <vector>

namespace vxe {

// CPU-side mesher output. Positions are chunk-local metres (|p| <= chunk size),
// so float precision is never stressed regardless of where the chunk lives.
struct TerrainVertex {
    Vec3f position;
    Vec3f normal;
    // Packed material: [0..7] primary id, [8..15] secondary id,
    // [16..23] secondary blend weight (unorm8), [24..31] cavity/AO (unorm8).
    uint32_t material = 0;
};

inline uint32_t packMaterial(MaterialId primary, MaterialId secondary, float blend, float ao) {
    return uint32_t(primary) | (uint32_t(secondary) << 8) | (uint32_t(saturate(blend) * 255.f + 0.5f) << 16) |
           (uint32_t(saturate(ao) * 255.f + 0.5f) << 24);
}

struct ChunkMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    Aabbf bounds{};
    // indices[0, skirtIndexStart) is the isosurface; anything after it is the
    // border skirt. Collision is built from the surface alone: the skirt is
    // interior geometry, and letting it in would move the simplifier's locked
    // border off the chunk boundary and unseal the join with the neighbour.
    size_t skirtIndexStart = 0;

    void clear() { vertices.clear(); indices.clear(); bounds = {}; skirtIndexStart = 0; }
    bool empty() const { return indices.empty(); }
    size_t triangleCount() const { return indices.size() / 3; }
    size_t surfaceIndexCount() const { return skirtIndexStart > 0 ? skirtIndexStart : indices.size(); }
};

// Mesh-shader cluster, layout-compatible with shaders/Common.hlsli `Meshlet`.
struct GpuMeshlet {
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint32_t vertexCount;
    uint32_t triangleCount;
};
// Culling bounds, layout-compatible with shaders/Common.hlsli `MeshletBounds`.
struct GpuMeshletBounds {
    float center[3];
    float radius;
    float coneApex[3];
    float coneCutoff;   // cos(half angle); >= 1 disables cone culling
    float coneAxis[3];
    float pad;
};
static_assert(sizeof(GpuMeshlet) == 16);
static_assert(sizeof(GpuMeshletBounds) == 48);

// Output of the meshoptimizer stage: render + collision representations.
struct OptimizedChunkMesh {
    // Render (vertex-cache + overdraw + fetch optimised)
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    // Mesh-shader path
    std::vector<GpuMeshlet> meshlets;
    std::vector<uint32_t> meshletVertices;     // indices into 'vertices'
    std::vector<uint8_t> meshletTriangles;     // 3 bytes per triangle, local to meshlet
    std::vector<GpuMeshletBounds> meshletBounds;
    // Physics (simplified with locked borders so neighbouring chunks stay sealed)
    std::vector<Vec3f> collisionVertices;
    std::vector<uint32_t> collisionIndices;

    Aabbf bounds{};
    bool empty() const { return indices.empty(); }
};

} // namespace vxe
