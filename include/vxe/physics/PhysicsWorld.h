#pragma once
// -----------------------------------------------------------------------------
// PhysicsWorld: owns the Jolt PhysicsSystem and its (optional) fixed-rate thread.
//
// Threading model
//   * The physics thread is the only writer of Jolt state. It runs:
//         preStep hooks  (apply baked terrain shapes, debris spawns, ...)
//         PhysicsSystem::Update(dt) on the shared TaskSystem via the adapter
//   * Other threads communicate through hooks' own queues; they never call
//     BodyInterface mutators directly.
//   * Queries (castRay) may come from any thread; they take a shared lock that
//     excludes only the Update() window (Jolt forbids queries during Update).
//
// Precision: Jolt is built with JPH_DOUBLE_PRECISION. Body positions are RVec3
// (double); shapes are float and chunk-local. No origin rebasing required.
// Engine headers stay free of Jolt types (pimpl); use joltSystem() from
// physics-module .cpp files only.
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace JPH { class PhysicsSystem; class BodyInterface; }

namespace vxe {

class TaskSystem;

namespace physics_layers {
inline constexpr uint16_t kStatic = 0;    // terrain chunks, placed structures
inline constexpr uint16_t kMoving = 1;    // characters, vehicles, debris
inline constexpr uint16_t kCount = 2;
}

struct PhysicsConfig {
    uint32_t maxBodies = 65536;
    uint32_t maxBodyPairs = 65536;
    uint32_t maxContactConstraints = 16384;
    uint32_t tempAllocatorBytes = 32u * 1024u * 1024u;
    float fixedHz = 60.f;
    int collisionSteps = 1;
};

struct RayHit {
    Vec3d point;
    Vec3f normal;
    float fraction;      // along the input ray
    uint32_t bodyId;
};

using BodyHandle = uint32_t;   // JPH::BodyID::GetIndexAndSequenceNumber()
inline constexpr BodyHandle kInvalidBody = 0xFFFFFFFFu;

class PhysicsWorld {
public:
    PhysicsWorld(TaskSystem& tasks, const PhysicsConfig& config = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // One fixed step: hooks, then Update. Call from the physics thread only
    // (or the main thread when no physics thread is running).
    void step();
    void startThread();
    void stopThread();
    bool threaded() const { return m_thread.joinable(); }
    uint64_t stepCount() const { return m_steps.load(std::memory_order_relaxed); }

    // Hooks run on the physics thread at the start of every step().
    using Hook = std::function<void(PhysicsWorld&)>;
    void addPreStepHook(Hook h);

    // Thread-safe query (shared lock).
    std::optional<RayHit> castRay(const Vec3d& origin, const Vec3f& direction, float maxDistance) const;

    // Convenience bodies (thread-safe through Jolt's locking BodyInterface,
    // but call outside step() for determinism).
    BodyHandle createDynamicSphere(const Vec3d& position, float radius, float density = 1000.f);
    BodyHandle createDynamicBox(const Vec3d& position, const Vec3f& halfExtents, float density = 600.f);
    void removeBody(BodyHandle h);
    Vec3d bodyPosition(BodyHandle h) const;
    bool bodyActive(BodyHandle h) const;
    uint32_t bodyCount() const;

    // Physics-module access (TerrainCollisionBaker, character controller, ...).
    JPH::PhysicsSystem& joltSystem();
    JPH::BodyInterface& bodyInterface();
    float fixedDt() const { return 1.f / m_config.fixedHz; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsConfig m_config;
    std::vector<Hook> m_hooks;
    std::mutex m_hookMutex;
    mutable std::shared_mutex m_queryLock;
    std::jthread m_thread;
    std::atomic<uint64_t> m_steps{0};
};

} // namespace vxe
