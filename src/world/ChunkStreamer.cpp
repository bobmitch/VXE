#include "vxe/world/ChunkStreamer.h"

#include "vxe/terraform/DeltaStore.h"

#include <algorithm>
#include <cstring>

namespace vxe {

namespace {

double chunkSizeMetres(uint8_t lod) { return double(world::kChunkCells << lod) * world::kVoxelSize; }

bool insideWorldXZ(const Aabbd& b) {
    const double e = world::kWorldHalfExtentXZ;
    return b.max.x > -e && b.min.x < e && b.max.z > -e && b.min.z < e;
}

} // namespace

ChunkStreamer::ChunkStreamer(TaskSystem& tasks, const DensityGenerator& generator, const DeltaStore& deltas,
                             const StreamerConfig& config)
    : m_tasks(tasks), m_generator(generator), m_deltas(deltas), m_config(config),
      m_fieldPool(16, (config.maxDensityFields + 15) / 16),
      m_results(std::make_shared<ConcurrentQueue<BuildResult>>()) {
    m_config.maxLod = std::min<uint8_t>(m_config.maxLod, world::kMaxLod);
}

ChunkStreamer::~ChunkStreamer() {
    m_tasks.waitAll(m_outstanding);
    m_results->drain();     // releases pooled fields before the pool dies
    for (auto& [k, r] : m_records) r.field.reset();
}

// ---------------------------------------------------------------------------
// Frame update
// ---------------------------------------------------------------------------
void ChunkStreamer::update(const Vec3d& focus) {
    ++m_frame;
    integrateResults();

    const Vec3d moved = focus - m_lastFocus;
    if (m_forceReselect || dot(moved, moved) > 16.0) {    // reselect after 4 m of travel
        computeDesired(focus);
        m_lastFocus = focus;
        m_forceReselect = false;
    }
    retireUndesired();
    dispatchBuilds();
    // Drop handles of finished graphs.
    std::erase_if(m_outstanding, [](const TaskHandle& h) { return h.isDone(); });
    refreshStats();
}

bool ChunkStreamer::isIdle() const {
    if (m_inFlight != 0 || m_results->sizeApprox() != 0) return false;
    for (const auto& [k, r] : m_records)
        if (r.desired && (!r.ready || r.rebuildQueued)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
void ChunkStreamer::computeDesired(const Vec3d& focus) {
    m_previousDesired.swap(m_desiredNodes);     // last frame's split decisions, for hysteresis
    m_desiredNodes.clear();
    for (auto& [k, r] : m_records) r.desired = false;

    const uint8_t top = m_config.maxLod;
    const double size = chunkSizeMetres(top);
    const double vd = m_config.viewDistance;
    const int32_t x0 = int32_t(std::floor((focus.x - vd) / size)), x1 = int32_t(std::floor((focus.x + vd) / size));
    const int32_t z0 = int32_t(std::floor((focus.z - vd) / size)), z1 = int32_t(std::floor((focus.z + vd) / size));
    const int32_t y0 = int32_t(std::floor(world::kWorldMinY / size));
    const int32_t y1 = int32_t(std::floor((world::kWorldMaxY - 1) / size));
    for (int32_t z = z0; z <= z1; ++z)
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x) visitNode(ChunkKey{x, y, z, top}, focus);
}

void ChunkStreamer::visitNode(const ChunkKey& key, const Vec3d& focus) {
    const Aabbd b = key.boundsWorld();
    if (!insideWorldXZ(b) || b.max.y <= world::kWorldMinY || b.min.y >= world::kWorldMaxY) return;
    const double dist = b.distanceTo(focus);
    if (dist > m_config.viewDistance) return;

    // Hysteresis: a node already split last frame holds on until the camera
    // pulls back past the wider threshold, so the boundary is sticky rather
    // than a step and a camera sitting on it stops rebuilding the subtree.
    double splitWithin = m_config.splitFactor * chunkSizeMetres(key.lod);
    if (const auto prev = m_previousDesired.find(key); prev != m_previousDesired.end() && !prev->second)
        splitWithin *= m_config.splitHysteresis;

    if (key.lod > 0 && dist < splitWithin) {
        m_desiredNodes[key] = false;
        for (int i = 0; i < 8; ++i) visitNode(key.child(i), focus);
        return;
    }

    m_desiredNodes[key] = true;
    auto [it, inserted] = m_records.try_emplace(key);
    Record& r = it->second;
    r.desired = true;
    r.undesiredFrames = 0;
    r.distance = dist;
    if (inserted) {
        r.key = key;
        r.priority = key.lod == 0 ? TaskPriority::High : TaskPriority::Normal;
        if (isLeafEmpty(key) && !m_deltas.hasEditsNear(key)) {
            r.empty = true;
            r.ready = true;
        }
    }
}

bool ChunkStreamer::isLeafEmpty(const ChunkKey& key) {
    const ChunkKey colKey{key.x, 0, key.z, key.lod};
    auto it = m_columnCache.find(colKey);
    if (it == m_columnCache.end()) {
        const Aabbd b = key.boundsWorld();
        // Include the padding ring so the envelope covers every sampled column.
        const double pad = double(key.strideVoxels()) * world::kVoxelSize * double(kFieldOffset + 1);
        const HeightRange hr = m_generator.columnRange(float(b.min.x - pad), float(b.min.z - pad),
                                                       float(b.max.x + pad), float(b.max.z + pad));
        it = m_columnCache.emplace(colKey, hr).first;
        if (m_columnCache.size() > 200000) m_columnCache.clear();   // bounded; cheap to rebuild
    }
    const Aabbd b = key.boundsWorld();
    const double pad = double(key.strideVoxels()) * world::kVoxelSize * double(kFieldOffset + 1);
    if (b.min.y - pad > it->second.maxY) return true;                                   // open sky
    if (key.lod > 0 && b.max.y + pad < it->second.minY - m_config.deepCullMargin) return true;  // deep rock, unseen
    return false;
}

// ---------------------------------------------------------------------------
// Retirement with coverage check
// ---------------------------------------------------------------------------
bool ChunkStreamer::descendantsReady(const ChunkKey& key) const {
    if (key.lod == 0) return true;
    for (int i = 0; i < 8; ++i) {
        const ChunkKey c = key.child(i);
        auto n = m_desiredNodes.find(c);
        if (n == m_desiredNodes.end()) continue;          // nothing desired in this octant
        if (n->second) {                                  // desired leaf
            auto r = m_records.find(c);
            if (r == m_records.end() || !r->second.ready) return false;
        } else if (!descendantsReady(c)) {
            return false;
        }
    }
    return true;
}

bool ChunkStreamer::isCovered(const ChunkKey& key) const {
    // A coarser desired leaf containing us?
    ChunkKey a = key;
    while (a.lod < m_config.maxLod) {
        a = a.parent();
        auto n = m_desiredNodes.find(a);
        if (n == m_desiredNodes.end()) continue;
        if (n->second) {
            auto r = m_records.find(a);
            return r != m_records.end() && r->second.ready;
        }
        break;  // an ancestor is split: our region is covered by finer leaves
    }
    // Finer desired leaves inside us (vacuously true if the region left the view).
    return descendantsReady(key);
}

void ChunkStreamer::retireUndesired() {
    for (auto it = m_records.begin(); it != m_records.end();) {
        Record& r = it->second;
        if (r.desired) { ++it; continue; }
        ++r.undesiredFrames;
        if (isCovered(r.key) || r.undesiredFrames > m_config.retireTimeoutFrames) {
            auto next = std::next(it);
            removeRecord(it);
            it = next;
        } else {
            ++it;
        }
    }
}

void ChunkStreamer::removeRecord(std::unordered_map<ChunkKey, Record, ChunkKeyHash>::iterator it) {
    const ChunkKey key = it->first;
    const bool hadMesh = it->second.mesh != nullptr;
    m_records.erase(it);  // in-flight results for this key will be discarded on arrival
    if (hadMesh)
        for (auto* l : m_listeners) l->onChunkRemoved(key);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
void ChunkStreamer::dispatchBuilds() {
    std::vector<Record*> candidates;
    for (auto& [k, r] : m_records) {
        if (r.inFlight || !r.desired) continue;
        if ((!r.ready && !r.empty) || r.rebuildQueued) candidates.push_back(&r);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Record* a, const Record* b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        if (a->key.lod != b->key.lod) return a->key.lod < b->key.lod;
        return a->distance < b->distance;
    });
    uint32_t dispatched = 0;
    for (Record* r : candidates) {
        if (m_inFlight >= m_config.maxJobsInFlight || dispatched >= m_config.maxDispatchPerUpdate) break;
        if (!dispatch(*r)) break;   // pool exhausted: back-pressure, retry next frame
        ++dispatched;
    }
}

bool ChunkStreamer::dispatch(Record& r) {
    // Shared state of one build graph. Each stage only touches its own outputs.
    struct BuildContext {
        ChunkKey key;
        uint32_t version;
        bool keepField;
        bool generate;
        PoolPtr<DensityField> field;
        ChunkMesh raw;
        std::shared_ptr<OptimizedChunkMesh> optimized = std::make_shared<OptimizedChunkMesh>();
        bool uniform = false;
    };

    const bool canRemesh = r.remeshOnly && r.field && r.key.lod == 0;
    PoolPtr<DensityField> field = m_fieldPool.acquire();
    if (!field) return false;

    auto ctx = std::make_shared<BuildContext>();
    ctx->key = r.key;
    ctx->version = r.requestedVersion = ++m_nextVersion;
    ctx->keepField = r.key.lod == 0 && !canRemesh;
    ctx->generate = !canRemesh;
    if (canRemesh) std::memcpy(field.get(), r.field.get(), sizeof(DensityField));   // snapshot; main owns r.field
    ctx->field = std::move(field);

    const TaskPriority prio = r.rebuildQueued ? r.priority : (r.key.lod == 0 ? TaskPriority::High : TaskPriority::Normal);
    r.inFlight = true;
    r.rebuildQueued = false;
    r.remeshOnly = false;
    ++m_inFlight;

    const DensityGenerator& gen = m_generator;
    const DeltaStore& deltas = m_deltas;
    const MesherSettings mesher = r.key.lod == 0 ? m_config.mesherLod0 : m_config.mesherFar;
    const MeshOptSettings meshOpt = [&] {
        MeshOptSettings s = m_config.meshOpt;
        s.buildCollision = r.key.lod == 0;   // only LOD0 is ever baked into Jolt
        return s;
    }();
    auto results = m_results;

    TaskHandle tGen;
    if (ctx->generate) {
        tGen = m_tasks.submit([ctx, &gen, &deltas] {
            const ChunkGenStats st = gen.generate(ctx->key, *ctx->field);
            const uint32_t applied = deltas.applyTo(ctx->key, *ctx->field);
            ctx->uniform = applied == 0 ? st.uniform : ctx->field->isUniform();
        }, prio, {}, "chunk.generate");
    } else {
        ctx->uniform = ctx->field->isUniform();
    }

    TaskHandle tMesh = m_tasks.submit([ctx, mesher] {
        if (!ctx->uniform)
            contourChunk(*ctx->field, float(ctx->key.strideVoxels()) * world::kVoxelSize, mesher, ctx->raw);
        // Uniform LOD0 fields are not kept resident (memory); edits regenerate them.
        if (!ctx->keepField || ctx->uniform) ctx->field.reset();
    }, prio, {tGen}, "chunk.contour");

    TaskHandle tOpt = m_tasks.submit([ctx, meshOpt, results] {
        optimizeChunkMesh(std::move(ctx->raw), meshOpt, *ctx->optimized);
        BuildResult res;
        res.key = ctx->key;
        res.version = ctx->version;
        res.field = std::move(ctx->field);
        res.mesh = std::move(ctx->optimized);
        res.uniform = ctx->uniform;
        res.generated = ctx->generate;
        results->push(std::move(res));
    }, prio, {tMesh}, "chunk.meshopt");

    m_outstanding.push_back(tOpt);
    return true;
}

// ---------------------------------------------------------------------------
// Integration (main thread)
// ---------------------------------------------------------------------------
void ChunkStreamer::integrateResults() {
    for (BuildResult& res : m_results->drain()) {
        --m_inFlight;
        auto it = m_records.find(res.key);
        if (it == m_records.end() || res.version != it->second.requestedVersion) {
            // Chunk retired (and possibly re-requested since, under a new global
            // version): the result is stale. Pooled memory returns on scope exit.
            ++m_stats.chunksDiscarded;
            continue;
        }
        Record& r = it->second;
        r.inFlight = false;
        r.ready = true;
        r.completedVersion = res.version;
        if (res.generated) {
            // The generated field (null for uniform or LOD>0 chunks) replaces the resident one.
            r.field = std::move(res.field);
            // Edits that raced with generation may be missing from this field;
            // the queued rebuild must regenerate (re-applying all deltas).
            if (r.rebuildQueued) r.remeshOnly = false;
        }
        r.mesh = std::move(res.mesh);
        ++m_stats.chunksBuilt;
        for (auto* l : m_listeners) l->onChunkMeshReady(r.key, r.completedVersion, r.mesh);
    }
}

void ChunkStreamer::invalidateRegion(const Aabbd& region, TaskPriority priority) {
    for (auto& [k, r] : m_records) {
        // Padding: a chunk's samples extend one stride beyond its bounds.
        const double pad = double(k.strideVoxels()) * world::kVoxelSize * double(kFieldOffset + 1);
        if (!k.boundsWorld().expanded(pad).intersects(region)) continue;
        r.rebuildQueued = true;
        r.priority = priority;
        r.remeshOnly = (k.lod == 0 && r.field != nullptr);
        if (r.empty) { r.empty = false; }   // edits can make sky/deep chunks non-empty
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
DensityField* ChunkStreamer::residentField(const ChunkKey& key) {
    auto it = m_records.find(key);
    return it == m_records.end() ? nullptr : it->second.field.get();
}
const DensityField* ChunkStreamer::residentField(const ChunkKey& key) const {
    auto it = m_records.find(key);
    return it == m_records.end() ? nullptr : it->second.field.get();
}

std::optional<float> ChunkStreamer::residentDensity(const Vec3i& v) const {
    constexpr int32_t n = world::kChunkCells;
    const ChunkKey owner{floorDiv(v.x, n), floorDiv(v.y, n), floorDiv(v.z, n), 0};
    if (const DensityField* f = residentField(owner)) {
        const Vec3i o = owner.originVoxel();
        return f->at(v.x - o.x, v.y - o.y, v.z - o.z);
    }
    return std::nullopt;
}

std::shared_ptr<const OptimizedChunkMesh> ChunkStreamer::meshFor(const ChunkKey& key) const {
    auto it = m_records.find(key);
    return it == m_records.end() ? nullptr : it->second.mesh;
}

void ChunkStreamer::refreshStats() {
    StreamerStats s;
    s.chunksBuilt = m_stats.chunksBuilt;
    s.chunksDiscarded = m_stats.chunksDiscarded;
    s.jobsInFlight = m_inFlight;
    for (const auto& [k, r] : m_records) {
        ++s.residentChunks;
        ++s.perLod[k.lod];
        if (r.empty) ++s.emptyChunks;
        if (r.ready) ++s.readyChunks;
        if (r.inFlight) ++s.buildingChunks;
        if (r.field) ++s.residentFields;
        if (r.mesh) s.triangles += r.mesh->indices.size() / 3;
    }
    m_stats = s;
}

} // namespace vxe
