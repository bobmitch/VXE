#pragma once
// -----------------------------------------------------------------------------
// TerrainCollisionBaker: asynchronous Jolt MeshShape baking for LOD0 chunks.
//
//   main thread              worker (TaskSystem)              physics thread
//   -----------              -------------------              --------------
//   onChunkMeshReady ----->  MeshShapeSettings + Create()     preStep hook:
//     latest[key] = v        (BVH build, ~0.5-2 ms)            drain queue
//                            push {key, v, ShapeRefC} ----->   if v == latest[key]:
//   onChunkRemoved  ------------------------------------------>   SetShape / batch-add
//     latest.erase(key)      push {key, remove}                  else drop (stale)
//
// Neither the main thread nor the render thread ever builds a shape or touches
// BodyInterface. New bodies are added in batches (AddBodiesPrepare/Finalize);
// shape swaps on existing bodies activate dynamic bodies in the chunk's AABB so
// objects resting on freshly carved terrain fall instead of floating.
// -----------------------------------------------------------------------------

#include "vxe/world/ChunkStreamer.h"

#include <memory>

namespace vxe {

class PhysicsWorld;
class TaskSystem;

struct CollisionBakerStats {
    uint64_t shapesBaked = 0, shapesApplied = 0, staleDropped = 0, bodiesAdded = 0, bodiesRemoved = 0;
    uint32_t liveBodies = 0;
    double lastBakeMs = 0.0;
};

class TerrainCollisionBaker final : public IChunkListener {
public:
    TerrainCollisionBaker(TaskSystem& tasks, PhysicsWorld& physics);
    ~TerrainCollisionBaker() override;

    // IChunkListener (main thread)
    void onChunkMeshReady(const ChunkKey& key, uint32_t version,
                          const std::shared_ptr<const OptimizedChunkMesh>& mesh) override;
    void onChunkRemoved(const ChunkKey& key) override;

    // Blocks until every submitted bake has been queued (tests/shutdown).
    void flush();
    CollisionBakerStats stats() const;
    bool hasBody(const ChunkKey& key) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vxe
