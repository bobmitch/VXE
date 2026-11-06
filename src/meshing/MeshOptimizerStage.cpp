#include "vxe/meshing/MeshOptimizerStage.h"

#include <algorithm>
#include <unordered_map>

#if VXE_WITH_MESHOPT
#include <meshoptimizer.h>
#endif

namespace vxe {

namespace {

void buildCollisionFromIndices(const std::vector<TerrainVertex>& verts, const std::vector<uint32_t>& idx,
                               OptimizedChunkMesh& out) {
    // Compact the position stream to only referenced vertices.
    std::vector<uint32_t> remap(verts.size(), ~0u);
    out.collisionVertices.clear();
    out.collisionIndices.clear();
    out.collisionIndices.reserve(idx.size());
    for (uint32_t i : idx) {
        if (remap[i] == ~0u) {
            remap[i] = uint32_t(out.collisionVertices.size());
            out.collisionVertices.push_back(verts[i].position);
        }
        out.collisionIndices.push_back(remap[i]);
    }
}

} // namespace

void optimizeChunkMesh(ChunkMesh&& in, const MeshOptSettings& s, OptimizedChunkMesh& out) {
    out = OptimizedChunkMesh{};
    out.bounds = in.bounds;
    if (in.empty()) return;

#if VXE_WITH_MESHOPT
    const size_t indexCount = in.indices.size();
    const size_t vertexCount = in.vertices.size();
    const float* positions = &in.vertices[0].position.x;
    constexpr size_t kStride = sizeof(TerrainVertex);

    // 1-3: cache, overdraw, fetch
    out.indices.resize(indexCount);
    meshopt_optimizeVertexCache(out.indices.data(), in.indices.data(), indexCount, vertexCount);
    meshopt_optimizeOverdraw(out.indices.data(), out.indices.data(), indexCount, positions, vertexCount, kStride,
                             s.overdrawThreshold);
    out.vertices.resize(vertexCount);
    const size_t used = meshopt_optimizeVertexFetch(out.vertices.data(), out.indices.data(), indexCount,
                                                    in.vertices.data(), vertexCount, kStride);
    out.vertices.resize(used);
    const float* optPositions = &out.vertices[0].position.x;

    // 4: meshlets
    if (s.buildMeshlets) {
        const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, s.meshletMaxVertices, s.meshletMaxTriangles);
        std::vector<meshopt_Meshlet> ml(maxMeshlets);
        out.meshletVertices.resize(maxMeshlets * s.meshletMaxVertices);
        out.meshletTriangles.resize(maxMeshlets * s.meshletMaxTriangles * 3);
        const size_t count = meshopt_buildMeshlets(ml.data(), out.meshletVertices.data(), out.meshletTriangles.data(),
                                                   out.indices.data(), indexCount, optPositions, used, kStride,
                                                   s.meshletMaxVertices, s.meshletMaxTriangles, s.meshletConeWeight);
        ml.resize(count);
        out.meshlets.reserve(count);
        out.meshletBounds.reserve(count);
        for (auto& m : ml) {
            meshopt_optimizeMeshlet(&out.meshletVertices[m.vertex_offset], &out.meshletTriangles[m.triangle_offset],
                                    m.triangle_count, m.vertex_count);
            const meshopt_Bounds b = meshopt_computeMeshletBounds(&out.meshletVertices[m.vertex_offset],
                                                                  &out.meshletTriangles[m.triangle_offset],
                                                                  m.triangle_count, optPositions, used, kStride);
            out.meshlets.push_back({m.vertex_offset, m.triangle_offset, m.vertex_count, m.triangle_count});
            GpuMeshletBounds gb{};
            std::copy_n(b.center, 3, gb.center);
            gb.radius = b.radius;
            std::copy_n(b.cone_apex, 3, gb.coneApex);
            std::copy_n(b.cone_axis, 3, gb.coneAxis);
            gb.coneCutoff = b.cone_cutoff;
            out.meshletBounds.push_back(gb);
        }
        if (!ml.empty()) {
            const auto& last = ml.back();
            out.meshletVertices.resize(last.vertex_offset + last.vertex_count);
            out.meshletTriangles.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3u));
        }
    }

    // 5: collision simplification with locked borders.
    // Built from the *surface* sub-mesh, in its pre-optimisation order, because
    // that is the only place the skirt can still be sliced off: the passes above
    // have reordered out.indices. The skirt must not reach the simplifier, or
    // SimplifyLockBorder would pin the collar's lower rim instead of the chunk
    // boundary and the neighbouring collision meshes would stop being sealed.
    if (s.buildCollision) {
        const size_t surfaceCount = in.surfaceIndexCount();
        std::vector<uint32_t> simplified(surfaceCount);
        const size_t target = size_t(float(surfaceCount) * s.collisionTargetRatio) / 3 * 3;
        float resultError = 0.f;
        const size_t n = meshopt_simplify(simplified.data(), in.indices.data(), surfaceCount, positions, vertexCount,
                                          kStride, target, s.collisionMaxErrorMetres,
                                          meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &resultError);
        if (n == 0) simplified.assign(in.indices.begin(), in.indices.begin() + ptrdiff_t(surfaceCount));
        else simplified.resize(n);
        buildCollisionFromIndices(in.vertices, simplified, out);
    }
#else
    const size_t surfaceCount = in.surfaceIndexCount();
    out.vertices = std::move(in.vertices);
    out.indices = std::move(in.indices);
    if (s.buildMeshlets) {
        // Greedy sequential clustering (no locality optimisation).
        std::unordered_map<uint32_t, uint8_t> local;
        GpuMeshlet cur{0, 0, 0, 0};
        auto flush = [&] {
            if (cur.triangleCount == 0) return;
            Aabbf bb{Vec3f(1e30f), Vec3f(-1e30f)};
            for (uint32_t i = 0; i < cur.vertexCount; ++i) {
                const Vec3f& p = out.vertices[out.meshletVertices[cur.vertexOffset + i]].position;
                bb.min = vmin(bb.min, p); bb.max = vmax(bb.max, p);
            }
            GpuMeshletBounds gb{};
            const Vec3f c = (bb.min + bb.max) * 0.5f;
            gb.center[0] = c.x; gb.center[1] = c.y; gb.center[2] = c.z;
            gb.radius = length(bb.max - c);
            gb.coneCutoff = 1.f;
            out.meshlets.push_back(cur);
            out.meshletBounds.push_back(gb);
            while (out.meshletTriangles.size() % 4) out.meshletTriangles.push_back(0);
            cur = {uint32_t(out.meshletVertices.size()), uint32_t(out.meshletTriangles.size()), 0, 0};
            local.clear();
        };
        for (size_t t = 0; t < out.indices.size(); t += 3) {
            int newVerts = 0;
            for (int k = 0; k < 3; ++k) newVerts += local.count(out.indices[t + size_t(k)]) ? 0 : 1;
            if (cur.vertexCount + uint32_t(newVerts) > s.meshletMaxVertices || cur.triangleCount + 1 > s.meshletMaxTriangles) flush();
            for (int k = 0; k < 3; ++k) {
                const uint32_t vi = out.indices[t + size_t(k)];
                auto [it, inserted] = local.try_emplace(vi, uint8_t(cur.vertexCount));
                if (inserted) { out.meshletVertices.push_back(vi); ++cur.vertexCount; }
                out.meshletTriangles.push_back(it->second);
            }
            ++cur.triangleCount;
        }
        flush();
    }
    if (s.buildCollision) {
        const std::vector<uint32_t> surface(out.indices.begin(), out.indices.begin() + ptrdiff_t(surfaceCount));
        buildCollisionFromIndices(out.vertices, surface, out);
    }
#endif
}

} // namespace vxe
