#include "vxe/render/TerrainRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe {

namespace {

// Octahedral normal encoding (Cigolle et al. 2014), snorm16 x2.
void encodeOctahedral(const Vec3f& n, int16_t out[2]) {
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    float x = n.x / l1, y = n.y / l1;
    if (n.z < 0.f) {
        const float ox = (1.f - std::abs(y)) * (x >= 0.f ? 1.f : -1.f);
        const float oy = (1.f - std::abs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = ox; y = oy;
    }
    out[0] = int16_t(std::lround(std::clamp(x, -1.f, 1.f) * 32767.f));
    out[1] = int16_t(std::lround(std::clamp(y, -1.f, 1.f) * 32767.f));
}

template <typename T>
uint64_t vecBytes(const std::vector<T>& v) { return uint64_t(v.size()) * sizeof(T); }

} // namespace

uint64_t TerrainRenderer::PackedChunk::bytes() const {
    return vecBytes(vertices) + vecBytes(indices) + vecBytes(meshlets) + vecBytes(meshletVertices) +
           vecBytes(meshletTriangleWords) + vecBytes(meshletBounds);
}

TerrainRenderer::TerrainRenderer(IRenderDevice& device, TaskSystem& tasks, const TerrainRendererConfig& cfg)
    : m_device(device), m_tasks(tasks), m_cfg(cfg), m_path(selectTerrainPath(device.caps())),
      m_vAlloc(cfg.vertexCapacity), m_iAlloc(cfg.indexCapacity), m_mAlloc(cfg.meshletCapacity),
      m_mvAlloc(cfg.meshletVertexCapacity), m_mtAlloc(cfg.meshletTriangleWordCapacity), m_ring(cfg.stagingBytes),
      m_packed(std::make_shared<ConcurrentQueue<std::shared_ptr<PackedChunk>>>()) {
    const uint32_t storage = kBufferStorage | kBufferTransferDst;
    m_buffers.vertices = device.createBuffer({cfg.vertexCapacity * sizeof(GpuTerrainVertex), storage | kBufferVertex, false, "terrain.vertices"});
    m_buffers.indices = device.createBuffer({cfg.indexCapacity * sizeof(uint32_t), storage | kBufferIndex, false, "terrain.indices"});
    m_buffers.meshlets = device.createBuffer({cfg.meshletCapacity * sizeof(GpuMeshlet), storage, false, "terrain.meshlets"});
    m_buffers.meshletBounds = device.createBuffer({cfg.meshletCapacity * sizeof(GpuMeshletBounds), storage, false, "terrain.meshletBounds"});
    m_buffers.meshletVertices = device.createBuffer({cfg.meshletVertexCapacity * sizeof(uint32_t), storage, false, "terrain.meshletVertices"});
    m_buffers.meshletTriangles = device.createBuffer({cfg.meshletTriangleWordCapacity * sizeof(uint32_t), storage, false, "terrain.meshletTriangles"});
    m_buffers.staging = device.createBuffer({cfg.stagingBytes, kBufferTransferSrc, true, "terrain.staging"});
    m_stagingPtr = static_cast<std::byte*>(device.map(m_buffers.staging));
}

TerrainRenderer::~TerrainRenderer() {
    flushPackJobs();
    for (BufferHandle h : {m_buffers.vertices, m_buffers.indices, m_buffers.meshlets, m_buffers.meshletBounds,
                           m_buffers.meshletVertices, m_buffers.meshletTriangles, m_buffers.staging})
        m_device.destroyBuffer(h);
}

void TerrainRenderer::flushPackJobs() {
    m_tasks.waitAll(m_jobs);
    m_jobs.clear();
}

// ---------------------------------------------------------------------------
// Packing (worker thread)
// ---------------------------------------------------------------------------
void TerrainRenderer::pack(const ChunkKey& key, uint32_t version, const OptimizedChunkMesh& mesh, PackedChunk& out) {
    out.key = key;
    out.version = version;
    out.bounds = mesh.bounds;
    const float step = float(key.strideVoxels()) * world::kVoxelSize;
    out.positionScale = float(kFieldCells + 2) * step;
    const float inv = 65535.f / out.positionScale;

    out.vertices.resize(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const TerrainVertex& s = mesh.vertices[i];
        GpuTerrainVertex& d = out.vertices[i];
        for (int k = 0; k < 3; ++k)
            d.pos[k] = uint16_t(std::clamp(std::lround(s.position[k] * inv), 0l, 65535l));
        d.flags = 0;
        encodeOctahedral(s.normal, d.nrm);
        d.material = s.material;
    }
    out.indices = mesh.indices;

    // Meshlets: re-base offsets to the chunk and pack triangles as one uint32 each.
    out.meshletVertices = mesh.meshletVertices;
    out.meshletBounds = mesh.meshletBounds;
    out.meshlets.reserve(mesh.meshlets.size());
    uint32_t triWord = 0;
    for (const GpuMeshlet& m : mesh.meshlets) {
        GpuMeshlet pm{m.vertexOffset, triWord, m.vertexCount, m.triangleCount};
        for (uint32_t t = 0; t < m.triangleCount; ++t) {
            const uint8_t* tri = &mesh.meshletTriangles[m.triangleOffset + t * 3];
            out.meshletTriangleWords.push_back(uint32_t(tri[0]) | (uint32_t(tri[1]) << 8) | (uint32_t(tri[2]) << 16));
        }
        triWord += m.triangleCount;
        out.meshlets.push_back(pm);
    }
}

void TerrainRenderer::onChunkMeshReady(const ChunkKey& key, uint32_t version,
                                       const std::shared_ptr<const OptimizedChunkMesh>& mesh) {
    m_latest[key] = version;
    if (!mesh || mesh->empty()) {          // became empty (fully carved / filled): drop GPU copy
        auto it = m_resident.find(key);
        if (it != m_resident.end()) { release(it->second); m_resident.erase(it); }
        return;
    }
    auto out = m_packed;
    std::erase_if(m_jobs, [](const TaskHandle& h) { return h.isDone(); });
    m_jobs.push_back(m_tasks.submit([key, version, mesh, out] {
        auto pc = std::make_shared<PackedChunk>();
        pack(key, version, *mesh, *pc);
        out->push(std::move(pc));
    }, key.lod == 0 ? TaskPriority::High : TaskPriority::Normal, {}, "terrain.pack"));
}

void TerrainRenderer::onChunkRemoved(const ChunkKey& key) {
    m_latest.erase(key);
    auto it = m_resident.find(key);
    if (it != m_resident.end()) { release(it->second); m_resident.erase(it); }
}

// ---------------------------------------------------------------------------
// Residency (render thread)
// ---------------------------------------------------------------------------
void TerrainRenderer::release(Resident& r) {
    const uint64_t frame = m_device.currentFrame();
    if (r.v.count) m_deferred.push(m_vAlloc, frame, r.v.offset, r.v.count);
    if (r.i.count) m_deferred.push(m_iAlloc, frame, r.i.offset, r.i.count);
    if (r.m.count) m_deferred.push(m_mAlloc, frame, r.m.offset, r.m.count);
    if (r.mv.count) m_deferred.push(m_mvAlloc, frame, r.mv.offset, r.mv.count);
    if (r.mt.count) m_deferred.push(m_mtAlloc, frame, r.mt.offset, r.mt.count);
    m_stats.vertexBytes -= r.v.count * sizeof(GpuTerrainVertex);
    m_stats.indexBytes -= r.i.count * sizeof(uint32_t);
    m_stats.meshletBytes -= r.m.count * (sizeof(GpuMeshlet) + sizeof(GpuMeshletBounds));
}

bool TerrainRenderer::stage(BufferHandle dst, uint64_t dstOffset, const void* src, uint64_t bytes) {
    if (bytes == 0) return true;
    const uint64_t off = m_ring.allocate(bytes, m_device.currentFrame());
    if (off == UploadRing::kInvalid) return false;
    std::memcpy(m_stagingPtr + off, src, bytes);
    m_device.copyBuffer(m_buffers.staging, off, dst, dstOffset, bytes);
    m_stats.uploadedBytes += bytes;
    return true;
}

bool TerrainRenderer::upload(PackedChunk& pc, uint64_t& budget) {
    const uint64_t bytes = pc.bytes();
    // Over-budget chunks are allowed as the first upload of a frame so they cannot starve.
    if (bytes > budget && budget != m_cfg.uploadBudgetPerFrame) return false;
    if (m_ring.capacity() - m_ring.liveBytes() < bytes + 256) return false;   // conservative: retry next frame

    Resident r;
    r.key = pc.key;
    r.version = pc.version;
    r.positionScale = pc.positionScale;
    r.bounds = pc.bounds;
    auto alloc = [](OffsetAllocator& a, Range& out, uint64_t n) {
        if (n == 0) return true;
        out.offset = a.allocate(n);
        out.count = n;
        return out.offset != OffsetAllocator::kInvalid;
    };
    const bool ok = alloc(m_vAlloc, r.v, pc.vertices.size()) && alloc(m_iAlloc, r.i, pc.indices.size()) &&
                    alloc(m_mAlloc, r.m, pc.meshlets.size()) && alloc(m_mvAlloc, r.mv, pc.meshletVertices.size()) &&
                    alloc(m_mtAlloc, r.mt, pc.meshletTriangleWords.size());
    if (!ok) {
        ++m_stats.allocFailures;
        auto undo = [](OffsetAllocator& a, Range& rg) { if (rg.offset != OffsetAllocator::kInvalid && rg.count) a.free(rg.offset, rg.count); };
        undo(m_vAlloc, r.v); undo(m_iAlloc, r.i); undo(m_mAlloc, r.m); undo(m_mvAlloc, r.mv); undo(m_mtAlloc, r.mt);
        return false;
    }

    bool staged = stage(m_buffers.vertices, r.v.offset * sizeof(GpuTerrainVertex), pc.vertices.data(), vecBytes(pc.vertices));
    staged &= stage(m_buffers.indices, r.i.offset * sizeof(uint32_t), pc.indices.data(), vecBytes(pc.indices));
    staged &= stage(m_buffers.meshlets, r.m.offset * sizeof(GpuMeshlet), pc.meshlets.data(), vecBytes(pc.meshlets));
    staged &= stage(m_buffers.meshletBounds, r.m.offset * sizeof(GpuMeshletBounds), pc.meshletBounds.data(), vecBytes(pc.meshletBounds));
    staged &= stage(m_buffers.meshletVertices, r.mv.offset * sizeof(uint32_t), pc.meshletVertices.data(), vecBytes(pc.meshletVertices));
    staged &= stage(m_buffers.meshletTriangles, r.mt.offset * sizeof(uint32_t), pc.meshletTriangleWords.data(), vecBytes(pc.meshletTriangleWords));
    (void)staged;   // capacity was pre-checked; a failure here would only drop bytes of a superseded frame

    budget -= std::min(budget, bytes);
    auto it = m_resident.find(pc.key);
    if (it != m_resident.end()) { release(it->second); it->second = r; }
    else m_resident.emplace(pc.key, r);
    m_stats.vertexBytes += r.v.count * sizeof(GpuTerrainVertex);
    m_stats.indexBytes += r.i.count * sizeof(uint32_t);
    m_stats.meshletBytes += r.m.count * (sizeof(GpuMeshlet) + sizeof(GpuMeshletBounds));
    return true;
}

void TerrainRenderer::update() {
    const uint64_t completed = m_device.completedFrame();
    m_ring.reclaim(completed);
    m_deferred.collect(completed);

    for (auto& pc : m_packed->drain()) m_waiting.push_back(std::move(pc));
    // Drop superseded / removed work, keep newest-first order for LOD0.
    std::erase_if(m_waiting, [this](const std::shared_ptr<PackedChunk>& pc) {
        auto it = m_latest.find(pc->key);
        return it == m_latest.end() || it->second != pc->version;
    });
    std::stable_sort(m_waiting.begin(), m_waiting.end(),
                     [](const auto& a, const auto& b) { return a->key.lod < b->key.lod; });

    uint64_t budget = m_cfg.uploadBudgetPerFrame;
    std::vector<std::shared_ptr<PackedChunk>> stillWaiting;
    for (auto& pc : m_waiting)
        if (!upload(*pc, budget)) stillWaiting.push_back(std::move(pc));
    m_waiting.swap(stillWaiting);

    m_stats.residentChunks = uint32_t(m_resident.size());
    m_stats.pendingUploads = uint32_t(m_waiting.size());
}

TerrainFrame TerrainRenderer::buildFrame(const Vec3d& cameraPos, const Frustum* frustum, float nearFieldDistance) const {
    TerrainFrame f;
    f.path = m_path;
    f.instances.reserve(m_resident.size());
    for (const auto& [key, r] : m_resident) {
        const Vec3f origin = toCameraRelative(key.originWorld(), cameraPos);     // double -> float, once per chunk
        const Aabbf boundsRel{origin + r.bounds.min, origin + r.bounds.max};
        if (frustum && !frustum->intersects(boundsRel)) continue;

        const uint32_t instanceIndex = uint32_t(f.instances.size());
        GpuChunkInstance inst{};
        inst.originCameraRelative[0] = origin.x;
        inst.originCameraRelative[1] = origin.y;
        inst.originCameraRelative[2] = origin.z;
        inst.positionScale = r.positionScale;
        inst.vertexBase = uint32_t(r.v.offset);
        inst.indexBase = uint32_t(r.i.count ? r.i.offset : 0);
        inst.meshletBase = uint32_t(r.m.count ? r.m.offset : 0);
        inst.meshletCount = uint32_t(r.m.count);
        inst.meshletVertexBase = uint32_t(r.mv.count ? r.mv.offset : 0);
        inst.meshletTriangleBase = uint32_t(r.mt.count ? r.mt.offset : 0);
        inst.lod = key.lod;
        inst.indexCount = uint32_t(r.i.count);
        const Vec3i ov = key.originVoxel();
        inst.noiseOrigin[0] = float(floorMod(ov.x, kNoisePeriodMetres)) * world::kVoxelSize;
        inst.noiseOrigin[1] = float(floorMod(ov.y, kNoisePeriodMetres)) * world::kVoxelSize;
        inst.noiseOrigin[2] = float(floorMod(ov.z, kNoisePeriodMetres)) * world::kVoxelSize;
        inst.flags = 0;
        f.instances.push_back(inst);

        if (m_path == TerrainPipelinePath::MeshShader) {
            const uint32_t groups = (inst.meshletCount + m_cfg.meshletsPerTaskGroup - 1) / m_cfg.meshletsPerTaskGroup;
            f.meshDispatches.push_back({groups, 1, 1, instanceIndex});
            const float dist = boundsRel.distanceTo(Vec3f(0.f));
            if (key.lod == 0 && dist < nearFieldDistance) f.nearFieldInstances.push_back(instanceIndex);
        } else {
            f.indexedDraws.push_back({inst.indexCount, 1, inst.indexBase, int32_t(inst.vertexBase), instanceIndex});
        }
    }
    return f;
}

} // namespace vxe
