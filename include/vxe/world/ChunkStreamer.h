#pragma once
// -----------------------------------------------------------------------------
// ChunkStreamer: owns the set of resident terrain chunks around a focus point
// and drives their asynchronous build pipelines.
//
// Selection: an implicit octree. Top-level chunks are LOD `maxLod` within
// viewDistance; a chunk is split into its 8 children while its distance to the
// focus is < splitFactor * chunkSize. The resulting leaves tile space with no
// overlaps. Leaves that are provably empty (above the terrain envelope, or deep
// rock at LOD > 0) are marked ready without any work.
//
// Hole-free transitions: a chunk that falls out of the desired set is only
// retired once every desired leaf overlapping it has a mesh (coverage check
// against the desired-node tree), or after a timeout.
//
// Build graph (per chunk, all on TaskSystem workers):
//     [generate + apply deltas] -> [dual contour] -> [meshoptimizer]
//     (remesh-only for LOD0 edits: snapshot resident field -> contour -> meshopt)
// Completed results come back through a ConcurrentQueue and are integrated on
// the main thread in update(); a version stamp discards superseded results.
//
// Memory: LOD0 chunks keep their DensityField resident (edits, anchor
// queries); all other LODs release the field right after contouring. Fields
// come from a BlockPool whose capacity is the hard streaming budget.
// -----------------------------------------------------------------------------

#include "vxe/core/Memory.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/meshing/DualContouring.h"
#include "vxe/meshing/MeshOptimizerStage.h"
#include "vxe/world/DensityGenerator.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vxe {

class DeltaStore;

struct StreamerConfig {
    uint8_t maxLod = 5;                 // 1024 m top-level chunks
    double viewDistance = 4096.0;       // metres
    double splitFactor = 2.0;           // split while dist < factor * chunkSize
    // Hysteresis on that threshold. A single threshold makes the split decision
    // a step function of distance, so a camera loitering on the boundary -- or
    // merely breathing across it, since `dist` is to the chunk AABB -- rebuilds
    // the same subtree every frame and the player watches one patch of ground
    // pop between two LODs. Once split, a node stays split until the camera
    // pulls back past splitFactor * chunkSize * this. It costs a band of chunks
    // held one level finer than strictly needed; 1.0 restores the old step.
    double splitHysteresis = 1.15;
    double deepCullMargin = 96.0;       // LOD>0 chunks this far below the surface envelope are skipped
    uint32_t maxJobsInFlight = 48;
    uint32_t maxDispatchPerUpdate = 24;
    uint32_t retireTimeoutFrames = 600;
    size_t maxDensityFields = 1024;     // hard budget: ~233 MB at 228 KB each
    // Trailing field is the border skirt depth in cells; it covers the inter-LOD
    // cracks, which the dual structure cannot close on its own. 4.5 cells is
    // measured, not guessed: across sampled LOD0|LOD1 seams the worst a rim ever
    // sits from the other side's surface is 4.04 m seen from the fine chunk
    // (4.5 m of reach) and 6.85 m from the coarse one (9 m of reach). See
    // lod_seam_cracks_are_covered_by_skirts, which asserts against this value.
    // Set both to 0 to mesh the bare isosurface.
    MesherSettings mesherLod0{1.0f, 0.1f, 0.25f, 4.5f};   // sharp DC near the player
    MesherSettings mesherFar{0.35f, 0.1f, 0.25f, 4.5f};   // softer, more Surface-Nets-like far away
    MeshOptSettings meshOpt{};
};

enum class ChunkState : uint8_t { Pending, Building, Ready };

struct StreamerStats {
    uint32_t residentChunks = 0, readyChunks = 0, buildingChunks = 0, emptyChunks = 0;
    uint32_t residentFields = 0;
    uint32_t jobsInFlight = 0;
    uint64_t chunksBuilt = 0, chunksDiscarded = 0;
    uint64_t triangles = 0;
    uint32_t perLod[world::kMaxLod + 1] = {};
};

// Main-thread notifications (render registry, collision baker, building system).
class IChunkListener {
public:
    virtual ~IChunkListener() = default;
    virtual void onChunkMeshReady(const ChunkKey& key, uint32_t version,
                                  const std::shared_ptr<const OptimizedChunkMesh>& mesh) = 0;
    virtual void onChunkRemoved(const ChunkKey& key) = 0;
};

class ChunkStreamer {
public:
    ChunkStreamer(TaskSystem& tasks, const DensityGenerator& generator, const DeltaStore& deltas,
                  const StreamerConfig& config = {});
    ~ChunkStreamer();

    void addListener(IChunkListener* l) { m_listeners.push_back(l); }

    // Main thread, once per frame.
    void update(const Vec3d& focus);
    // True when nothing is pending/building and the desired set is fully ready.
    bool isIdle() const;

    // Marks every resident chunk (all LODs) overlapping `region` for rebuild.
    // LOD0 chunks with a resident field are re-contoured from that field
    // (call after patching it); other LODs regenerate from the generator+deltas.
    void invalidateRegion(const Aabbd& region, TaskPriority priority = TaskPriority::Critical);

    // Resident LOD0 field access for terraforming and proximity queries (main thread).
    DensityField* residentField(const ChunkKey& lod0Key);
    const DensityField* residentField(const ChunkKey& lod0Key) const;
    // Sample density at a LOD0 voxel corner from resident data if possible.
    std::optional<float> residentDensity(const Vec3i& voxel) const;

    const StreamerStats& stats() const { return m_stats; }
    const StreamerConfig& config() const { return m_config; }
    std::shared_ptr<const OptimizedChunkMesh> meshFor(const ChunkKey& key) const;
    template <typename F> void forEachReady(F&& f) const {
        for (const auto& [k, r] : m_records)
            if (r.ready && r.mesh && !r.mesh->empty()) f(k, *r.mesh);
    }

private:
    struct Record {
        ChunkKey key{};
        bool desired = false;
        bool ready = false;           // has completed at least one build (or is empty)
        bool empty = false;           // culled: ready without a mesh
        bool inFlight = false;
        bool rebuildQueued = false;   // dirtied: needs another build
        bool remeshOnly = false;      // next build may reuse the resident field
        TaskPriority priority = TaskPriority::Normal;
        uint32_t requestedVersion = 0, completedVersion = 0;
        uint32_t undesiredFrames = 0;
        double distance = 0.0;
        PoolPtr<DensityField> field{};
        std::shared_ptr<const OptimizedChunkMesh> mesh;
        ChunkState state() const { return inFlight && !ready ? ChunkState::Building : (ready ? ChunkState::Ready : ChunkState::Pending); }
    };
    struct BuildResult {
        ChunkKey key{};
        uint32_t version = 0;
        PoolPtr<DensityField> field{};
        std::shared_ptr<OptimizedChunkMesh> mesh;
        bool uniform = false;
        bool generated = false;
    };

    void integrateResults();
    void computeDesired(const Vec3d& focus);
    void visitNode(const ChunkKey& key, const Vec3d& focus);
    bool isLeafEmpty(const ChunkKey& key);
    void retireUndesired();
    bool isCovered(const ChunkKey& key) const;
    bool descendantsReady(const ChunkKey& key) const;
    void dispatchBuilds();
    bool dispatch(Record& r);
    void removeRecord(std::unordered_map<ChunkKey, Record, ChunkKeyHash>::iterator it);
    void refreshStats();

    TaskSystem& m_tasks;
    const DensityGenerator& m_generator;
    const DeltaStore& m_deltas;
    StreamerConfig m_config;
    BlockPool<DensityField> m_fieldPool;

    std::unordered_map<ChunkKey, Record, ChunkKeyHash> m_records;
    // Desired octree nodes of the current frame: true = leaf, false = split.
    // key -> was a desired leaf (true) or split into children (false).
    std::unordered_map<ChunkKey, bool, ChunkKeyHash> m_desiredNodes;
    std::unordered_map<ChunkKey, bool, ChunkKeyHash> m_previousDesired;   // last frame's, for hysteresis
    // XZ column envelope cache, keyed by (x, 0, z, lod).
    std::unordered_map<ChunkKey, HeightRange, ChunkKeyHash> m_columnCache;

    std::shared_ptr<ConcurrentQueue<BuildResult>> m_results;
    std::vector<IChunkListener*> m_listeners;
    uint32_t m_inFlight = 0;
    uint32_t m_nextVersion = 0;   // global, so a re-created record never matches a stale result
    std::vector<TaskHandle> m_outstanding;   // tail task of every in-flight graph
    Vec3d m_lastFocus{1e30, 1e30, 1e30};
    bool m_forceReselect = true;
    uint64_t m_frame = 0;
    StreamerStats m_stats;
};

} // namespace vxe
