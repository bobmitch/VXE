#include "vxe/physics/PhysicsWorld.h"

#include "vxe/core/TaskSystem.h"
#include "vxe/physics/JoltJobSystemAdapter.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace vxe {

namespace {

// ---- Process-wide Jolt registration (ref-counted) -------------------------
std::mutex g_joltInitMutex;
int g_joltInitCount = 0;

void joltTrace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

void acquireJolt() {
    std::lock_guard lock(g_joltInitMutex);
    if (g_joltInitCount++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}
void releaseJolt() {
    std::lock_guard lock(g_joltInitMutex);
    if (--g_joltInitCount == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

// ---- Layers ------------------------------------------------------------------
namespace bp {
inline constexpr JPH::BroadPhaseLayer kStatic(0);
inline constexpr JPH::BroadPhaseLayer kMoving(1);
inline constexpr JPH::uint kCount = 2;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return bp::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == physics_layers::kStatic ? bp::kStatic : bp::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp::kStatic ? "STATIC" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpl) const override {
        return layer == physics_layers::kStatic ? bpl == bp::kMoving : true;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == physics_layers::kStatic && b == physics_layers::kStatic);
    }
};

JPH::BodyID toBodyId(BodyHandle h) { return JPH::BodyID(h); }

} // namespace

struct PhysicsWorld::Impl {
    explicit Impl(TaskSystem& tasks, const PhysicsConfig& cfg)
        : temp(cfg.tempAllocatorBytes), jobs(tasks) {
        system.Init(cfg.maxBodies, 0, cfg.maxBodyPairs, cfg.maxContactConstraints, bpLayers, objVsBp, pairs);
        system.SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));
    }
    BroadPhaseLayers bpLayers;
    ObjectVsBroadPhase objVsBp;
    ObjectPairs pairs;
    JPH::TempAllocatorImpl temp;
    JoltJobSystemAdapter jobs;
    JPH::PhysicsSystem system;
};

PhysicsWorld::PhysicsWorld(TaskSystem& tasks, const PhysicsConfig& config) : m_config(config) {
    acquireJolt();
    m_impl = std::make_unique<Impl>(tasks, config);
}

PhysicsWorld::~PhysicsWorld() {
    stopThread();
    // Remove and destroy every body before the system goes away.
    JPH::BodyIDVector ids;
    m_impl->system.GetBodies(ids);
    auto& bi = m_impl->system.GetBodyInterface();
    for (const JPH::BodyID& id : ids) {
        if (bi.IsAdded(id)) bi.RemoveBody(id);
        bi.DestroyBody(id);
    }
    m_impl.reset();
    releaseJolt();
}

void PhysicsWorld::addPreStepHook(Hook h) {
    std::lock_guard lock(m_hookMutex);
    m_hooks.push_back(std::move(h));
}

void PhysicsWorld::step() {
    {
        std::lock_guard lock(m_hookMutex);
        for (auto& h : m_hooks) h(*this);
    }
    {
        std::unique_lock lock(m_queryLock);   // queries are illegal during Update
        m_impl->system.Update(fixedDt(), m_config.collisionSteps, &m_impl->temp, &m_impl->jobs);
    }
    m_steps.fetch_add(1, std::memory_order_relaxed);
}

void PhysicsWorld::startThread() {
    if (m_thread.joinable()) return;
    m_thread = std::jthread([this](std::stop_token st) {
        using clock = std::chrono::steady_clock;
        const auto dt = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(fixedDt()));
        auto next = clock::now();
        while (!st.stop_requested()) {
            step();
            next += dt;
            const auto now = clock::now();
            if (next < now - dt * 4) next = now;   // spiral-of-death guard: drop steps rather than catch up forever
            std::this_thread::sleep_until(next);
        }
    });
}

void PhysicsWorld::stopThread() {
    if (!m_thread.joinable()) return;
    m_thread.request_stop();
    m_thread.join();
}

std::optional<RayHit> PhysicsWorld::castRay(const Vec3d& origin, const Vec3f& direction, float maxDistance) const {
    const Vec3f d = normalize(direction) * maxDistance;
    const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), JPH::Vec3(d.x, d.y, d.z)};
    JPH::RayCastResult hit;
    std::shared_lock lock(m_queryLock);
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, hit)) return std::nullopt;

    RayHit out{};
    out.fraction = hit.mFraction;
    const JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
    out.point = {double(p.GetX()), double(p.GetY()), double(p.GetZ())};
    out.bodyId = hit.mBodyID.GetIndexAndSequenceNumber();
    JPH::BodyLockRead bl(m_impl->system.GetBodyLockInterface(), hit.mBodyID);
    if (bl.Succeeded()) {
        const JPH::Vec3 n = bl.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p);
        out.normal = {n.GetX(), n.GetY(), n.GetZ()};
    }
    return out;
}

BodyHandle PhysicsWorld::createDynamicSphere(const Vec3d& pos, float radius, float density) {
    JPH::SphereShapeSettings ss(radius);
    ss.SetDensity(density);
    auto shape = ss.Create();
    if (shape.HasError()) return kInvalidBody;
    JPH::BodyCreationSettings bcs(shape.Get(), JPH::RVec3(pos.x, pos.y, pos.z), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic, physics_layers::kMoving);
    const JPH::BodyID id = m_impl->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
    return id.IsInvalid() ? kInvalidBody : id.GetIndexAndSequenceNumber();
}

BodyHandle PhysicsWorld::createDynamicBox(const Vec3d& pos, const Vec3f& he, float density) {
    JPH::BoxShapeSettings bs(JPH::Vec3(he.x, he.y, he.z));
    bs.SetDensity(density);
    auto shape = bs.Create();
    if (shape.HasError()) return kInvalidBody;
    JPH::BodyCreationSettings bcs(shape.Get(), JPH::RVec3(pos.x, pos.y, pos.z), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic, physics_layers::kMoving);
    const JPH::BodyID id = m_impl->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
    return id.IsInvalid() ? kInvalidBody : id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::removeBody(BodyHandle h) {
    auto& bi = m_impl->system.GetBodyInterface();
    const JPH::BodyID id = toBodyId(h);
    if (bi.IsAdded(id)) bi.RemoveBody(id);
    bi.DestroyBody(id);
}

Vec3d PhysicsWorld::bodyPosition(BodyHandle h) const {
    const JPH::RVec3 p = m_impl->system.GetBodyInterface().GetPosition(toBodyId(h));
    return {double(p.GetX()), double(p.GetY()), double(p.GetZ())};
}
bool PhysicsWorld::bodyActive(BodyHandle h) const { return m_impl->system.GetBodyInterface().IsActive(toBodyId(h)); }
uint32_t PhysicsWorld::bodyCount() const { return m_impl->system.GetNumBodies(); }

JPH::PhysicsSystem& PhysicsWorld::joltSystem() { return m_impl->system; }
JPH::BodyInterface& PhysicsWorld::bodyInterface() { return m_impl->system.GetBodyInterface(); }

} // namespace vxe
