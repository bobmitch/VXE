#pragma once
// -----------------------------------------------------------------------------
// Terrain GPU residency + draw list construction.
//
// GPU vertex (16 bytes, matches shaders/Common.hlsli `PackedVertex`):
//     uint16 pos[3]   chunk-local position, unorm over [0, 34 * voxelStep]
//                     (0.52 mm at LOD0, 1.7 cm at LOD5)
//     uint16 flags    reserved (edit age for wet/fresh-dig decals)
//     int16  nrm[2]   octahedral normal, snorm16
//     uint32 material packMaterial(): primary, secondary, blend, cavity
//
// Every chunk lives in shared mega-buffers (vertices, indices, meshlets,
// meshlet vertex/triangle streams, meshlet bounds). A chunk instance record
// carries its base offsets and its *camera-relative* origin, computed in
// double precision every frame, so vertex shaders only ever see small floats.
//
// Threading: onChunkMeshReady() (main) submits a pack job (quantisation) to the
// TaskSystem; update() (render-submit thread, here == main) integrates packed
// chunks, sub-allocates ranges and stages copies through the UploadRing under a
// per-frame byte budget. Freed ranges are recycled only after the GPU retires
// the frames that referenced them.
// -----------------------------------------------------------------------------

#include "vxe/core/Memory.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/render/GpuAllocators.h"
#include "vxe/render/RenderDevice.h"
#include "vxe/world/ChunkStreamer.h"

#include <array>
#include <unordered_map>

namespace vxe {

struct GpuTerrainVertex {
    uint16_t pos[3];
    uint16_t flags;
    int16_t nrm[2];
    uint32_t material;
};
static_assert(sizeof(GpuTerrainVertex) == 16);

// Matches shaders/Common.hlsli `ChunkInstance` (std430 / StructuredBuffer).
struct GpuChunkInstance {
    float originCameraRelative[3];
    float positionScale;            // metres per unorm16 step * 65535 (i.e. quantisation range)
    uint32_t vertexBase;
    uint32_t indexBase;
    uint32_t meshletBase;
    uint32_t meshletCount;
    uint32_t meshletVertexBase;
    uint32_t meshletTriangleBase;   // in bytes / 4 (uint32 words)
    uint32_t lod;
    uint32_t indexCount;
    // Chunk origin modulo kNoisePeriod (exact integers). Shaders evaluate world
    // space noise / triplanar UVs at noiseOrigin + localPos, which stays < 5.2 km
    // anywhere in the world -> sub-mm float precision. Noise is periodic at
    // kNoisePeriod, so the wrap is seamless.
    float noiseOrigin[3];
    uint32_t flags;
};
static_assert(sizeof(GpuChunkInstance) == 64);
inline constexpr int32_t kNoisePeriodMetres = 4096;   // must match shaders/Noise.hlsli

// D3D12_DRAW_INDEXED_ARGUMENTS / VkDrawIndexedIndirectCommand
struct DrawIndexedIndirectArgs {
    uint32_t indexCount, instanceCount, firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
};
// D3D12_DISPATCH_MESH_ARGUMENTS / VkDrawMeshTasksIndirectCommandEXT (+ draw id)
struct DispatchMeshIndirectArgs {
    uint32_t groupsX, groupsY, groupsZ;
    uint32_t instanceIndex;   // root constant / push constant written via ExecuteIndirect
};

struct Plane { Vec3f n; float d; };
struct Frustum {
    std::array<Plane, 6> planes;   // camera-relative space, normals point inward
    bool intersects(const Aabbf& b) const {
        for (const Plane& p : planes) {
            const Vec3f v{p.n.x >= 0 ? b.max.x : b.min.x, p.n.y >= 0 ? b.max.y : b.min.y, p.n.z >= 0 ? b.max.z : b.min.z};
            if (dot(p.n, v) + p.d < 0.f) return false;
        }
        return true;
    }
};

struct TerrainRendererConfig {
    uint64_t vertexCapacity = 8u << 20;          // 8M vertices  (128 MB)
    uint64_t indexCapacity = 32u << 20;          // 32M indices  (128 MB)
    uint64_t meshletCapacity = 512u << 10;
    uint64_t meshletVertexCapacity = 32u << 20;
    uint64_t meshletTriangleWordCapacity = 32u << 20;
    uint64_t stagingBytes = 64u << 20;
    uint64_t uploadBudgetPerFrame = 16u << 20;
    uint32_t meshletsPerTaskGroup = 32;          // must match TerrainMesh.as.hlsl
};

struct TerrainFrame {
    std::vector<GpuChunkInstance> instances;
    std::vector<DrawIndexedIndirectArgs> indexedDraws;     // Tessellation / Classic paths
    std::vector<DispatchMeshIndirectArgs> meshDispatches;  // Mesh-shader path
    std::vector<uint32_t> nearFieldInstances;              // chunks needing compute displacement (MS path)
    TerrainPipelinePath path = TerrainPipelinePath::Classic;
};

struct TerrainRendererStats {
    uint32_t residentChunks = 0, pendingUploads = 0, allocFailures = 0;
    uint64_t vertexBytes = 0, indexBytes = 0, meshletBytes = 0, uploadedBytes = 0;
};

class TerrainRenderer final : public IChunkListener {
public:
    TerrainRenderer(IRenderDevice& device, TaskSystem& tasks, const TerrainRendererConfig& cfg = {});
    ~TerrainRenderer() override;

    void onChunkMeshReady(const ChunkKey& key, uint32_t version,
                          const std::shared_ptr<const OptimizedChunkMesh>& mesh) override;
    void onChunkRemoved(const ChunkKey& key) override;

    // Render thread, after device.beginFrame().
    void update();
    TerrainFrame buildFrame(const Vec3d& cameraPos, const Frustum* frustum, float nearFieldDistance) const;

    const TerrainRendererStats& stats() const { return m_stats; }
    TerrainPipelinePath path() const { return m_path; }
    void flushPackJobs();

    struct Buffers {
        BufferHandle vertices, indices, meshlets, meshletVertices, meshletTriangles, meshletBounds, staging;
    };
    const Buffers& buffers() const { return m_buffers; }

private:
    struct PackedChunk {
        ChunkKey key{};
        uint32_t version = 0;
        float positionScale = 0.f;
        Aabbf bounds{};
        std::vector<GpuTerrainVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<GpuMeshlet> meshlets;
        std::vector<uint32_t> meshletVertices;
        std::vector<uint32_t> meshletTriangleWords;
        std::vector<GpuMeshletBounds> meshletBounds;
        uint64_t bytes() const;
    };
    struct Range { uint64_t offset = OffsetAllocator::kInvalid, count = 0; };
    struct Resident {
        ChunkKey key{};
        uint32_t version = 0;
        float positionScale = 0.f;
        Aabbf bounds{};
        Range v, i, m, mv, mt, mb;   // mb shares the meshlet range index
    };

    static void pack(const ChunkKey& key, uint32_t version, const OptimizedChunkMesh& mesh, PackedChunk& out);
    bool upload(PackedChunk& pc, uint64_t& budget);
    void release(Resident& r);
    bool stage(BufferHandle dst, uint64_t dstOffset, const void* src, uint64_t bytes);

    IRenderDevice& m_device;
    TaskSystem& m_tasks;
    TerrainRendererConfig m_cfg;
    TerrainPipelinePath m_path;
    Buffers m_buffers;
    std::byte* m_stagingPtr = nullptr;

    OffsetAllocator m_vAlloc, m_iAlloc, m_mAlloc, m_mvAlloc, m_mtAlloc;
    DeferredFrees m_deferred;
    UploadRing m_ring;

    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> m_latest;     // newest requested version
    std::unordered_map<ChunkKey, Resident, ChunkKeyHash> m_resident;
    std::shared_ptr<ConcurrentQueue<std::shared_ptr<PackedChunk>>> m_packed;
    std::vector<std::shared_ptr<PackedChunk>> m_waiting;                // packed but not yet uploaded
    std::vector<TaskHandle> m_jobs;
    TerrainRendererStats m_stats;
};

} // namespace vxe
