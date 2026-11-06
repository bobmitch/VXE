#include "vxe/physics/TerrainCollisionBaker.h"

#include "vxe/core/Memory.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace vxe {

namespace {
struct ShapeUpdate {
    ChunkKey key{};
    uint32_t version = 0;
    JPH::RefConst<JPH::Shape> shape;   // null => remove body
};
} // namespace

struct TerrainCollisionBaker::Impl {
    Impl(TaskSystem& t, PhysicsWorld& p) : tasks(t), physics(p) {}

    TaskSystem& tasks;
    PhysicsWorld& physics;

    // main thread writes, physics thread reads
    mutable std::mutex latestMutex;
    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> latest;

    ConcurrentQueue<ShapeUpdate> queue;
    std::vector<TaskHandle> outstanding;   // main thread only

    // physics thread only
    std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash> bodies;

    std::atomic<uint64_t> baked{0}, applied{0}, stale{0}, added{0}, removed{0};
    std::atomic<uint32_t> live{0};
    std::atomic<double> lastBakeMs{0.0};

    bool isLatest(const ChunkKey& k, uint32_t v) const {
        std::lock_guard lock(latestMutex);
        auto it = latest.find(k);
        return it != latest.end() && it->second == v;
    }

    void applyOnPhysicsThread(PhysicsWorld& world) {
        std::vector<ShapeUpdate> updates = queue.drain();
        if (updates.empty()) return;
        JPH::BodyInterface& bi = world.bodyInterface();
        JPH::BodyIDVector toAdd;

        for (ShapeUpdate& u : updates) {
            auto bodyIt = bodies.find(u.key);
            if (!u.shape) {
                if (bodyIt != bodies.end()) {
                    // A body queued for batch-add this frame must be added before removal.
                    if (bi.IsAdded(bodyIt->second)) bi.RemoveBody(bodyIt->second);
                    else toAdd.erase(std::remove(toAdd.begin(), toAdd.end(), bodyIt->second), toAdd.end());
                    bi.DestroyBody(bodyIt->second);
                    bodies.erase(bodyIt);
                    ++removed;
                }
                continue;
            }
            if (!isLatest(u.key, u.version)) { ++stale; continue; }

            const Aabbd b = u.key.boundsWorld();
            if (bodyIt != bodies.end()) {
                bi.SetShape(bodyIt->second, u.shape, false, JPH::EActivation::DontActivate);
                // Wake anything resting on/near the changed terrain.
                const JPH::AABox box(JPH::Vec3(float(b.min.x), float(b.min.y), float(b.min.z)) - JPH::Vec3::sReplicate(1.f),
                                     JPH::Vec3(float(b.max.x), float(b.max.y), float(b.max.z)) + JPH::Vec3::sReplicate(1.f));
                bi.ActivateBodiesInAABox(box, {}, {});
            } else {
                JPH::BodyCreationSettings bcs(u.shape, JPH::RVec3(b.min.x, b.min.y, b.min.z), JPH::Quat::sIdentity(),
                                              JPH::EMotionType::Static, physics_layers::kStatic);
                JPH::Body* body = bi.CreateBody(bcs);
                if (!body) continue;   // body limit reached; next rebuild retries
                bodies.emplace(u.key, body->GetID());
                toAdd.push_back(body->GetID());
            }
            ++applied;
        }
        if (!toAdd.empty()) {
            const JPH::BodyInterface::AddState st = bi.AddBodiesPrepare(toAdd.data(), int(toAdd.size()));
            bi.AddBodiesFinalize(toAdd.data(), int(toAdd.size()), st, JPH::EActivation::DontActivate);
            added += toAdd.size();
            if (toAdd.size() > 32) world.joltSystem().OptimizeBroadPhase();
        }
        live = uint32_t(bodies.size());
    }
};

TerrainCollisionBaker::TerrainCollisionBaker(TaskSystem& tasks, PhysicsWorld& physics)
    : m_impl(std::make_unique<Impl>(tasks, physics)) {
    Impl* impl = m_impl.get();
    // NB: the baker must outlive the PhysicsWorld's last step (owners destroy the
    // baker after stopping the physics thread; see apps/vxe_demo).
    physics.addPreStepHook([impl](PhysicsWorld& w) { impl->applyOnPhysicsThread(w); });
}

TerrainCollisionBaker::~TerrainCollisionBaker() { flush(); }

void TerrainCollisionBaker::onChunkMeshReady(const ChunkKey& key, uint32_t version,
                                             const std::shared_ptr<const OptimizedChunkMesh>& mesh) {
    if (key.lod != 0) return;   // only full-resolution terrain collides
    Impl& im = *m_impl;
    {
        std::lock_guard lock(im.latestMutex);
        im.latest[key] = version;
    }
    if (!mesh || mesh->collisionIndices.empty()) {
        im.queue.push(ShapeUpdate{key, version, nullptr});
        return;
    }
    std::erase_if(im.outstanding, [](const TaskHandle& h) { return h.isDone(); });
    im.outstanding.push_back(im.tasks.submit([&im, key, version, mesh] {
        if (!im.isLatest(key, version)) { ++im.stale; return; }   // superseded before we started
        const auto t0 = std::chrono::steady_clock::now();

        JPH::VertexList verts;
        verts.reserve(mesh->collisionVertices.size());
        for (const Vec3f& v : mesh->collisionVertices) verts.push_back(JPH::Float3(v.x, v.y, v.z));
        JPH::IndexedTriangleList tris;
        tris.reserve(mesh->collisionIndices.size() / 3);
        for (size_t i = 0; i + 2 < mesh->collisionIndices.size(); i += 3)
            tris.push_back(JPH::IndexedTriangle(mesh->collisionIndices[i], mesh->collisionIndices[i + 1],
                                                mesh->collisionIndices[i + 2], 0));

        JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));   // sanitises degenerates
        settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;  // rebuilt often
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) return;

        im.lastBakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++im.baked;
        im.queue.push(ShapeUpdate{key, version, result.Get()});
    }, TaskPriority::High, {}, "collision.bake"));
}

void TerrainCollisionBaker::onChunkRemoved(const ChunkKey& key) {
    if (key.lod != 0) return;
    {
        std::lock_guard lock(m_impl->latestMutex);
        m_impl->latest.erase(key);
    }
    m_impl->queue.push(ShapeUpdate{key, 0, nullptr});
}

void TerrainCollisionBaker::flush() {
    m_impl->tasks.waitAll(m_impl->outstanding);
    m_impl->outstanding.clear();
}

CollisionBakerStats TerrainCollisionBaker::stats() const {
    CollisionBakerStats s;
    s.shapesBaked = m_impl->baked;
    s.shapesApplied = m_impl->applied;
    s.staleDropped = m_impl->stale;
    s.bodiesAdded = m_impl->added;
    s.bodiesRemoved = m_impl->removed;
    s.liveBodies = m_impl->live;
    s.lastBakeMs = m_impl->lastBakeMs;
    return s;
}

bool TerrainCollisionBaker::hasBody(const ChunkKey& key) const {
    std::lock_guard lock(m_impl->latestMutex);
    return m_impl->latest.count(key) != 0;
}

} // namespace vxe
