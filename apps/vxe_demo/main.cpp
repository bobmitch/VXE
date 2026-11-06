// =============================================================================
// vxe_demo - headless end-to-end run of the engine pipeline:
//
//   stream world around a spawn point  ->  drive 400 m at vehicle speed
//   ->  carve terrain (remesh + Jolt re-bake + physics reaction)
//   ->  build on terrain, dig out the foundation, watch it collapse
//   ->  save / reload deltas  ->  animate a character with foot IK
//   ->  build a GPU frame (instances + indirect args) on the Null RHI
//
// Everything runs through the same code paths a windowed client would use;
// only the RHI backend is the CPU NullRenderDevice.
// =============================================================================
// The VXE_WITH_* macros are only defined when the matching option is ON, which
// is fine for #if but not for the C++ expressions below, so give them a value.
#ifndef VXE_WITH_JOLT
#define VXE_WITH_JOLT 0
#endif
#ifndef VXE_WITH_OZZ
#define VXE_WITH_OZZ 0
#endif

#include "vxe/building/BuildingSystem.h"
#include "vxe/core/TaskSystem.h"
#include "vxe/render/MaterialSystem.h"
#include "vxe/render/TerrainRenderer.h"
#include "vxe/terraform/DeltaStore.h"
#include "vxe/terraform/Terraformer.h"
#include "vxe/world/ChunkStreamer.h"
#include "vxe/world/DensityGenerator.h"
#if VXE_WITH_JOLT
#include "vxe/physics/PhysicsWorld.h"
#include "vxe/physics/TerrainCollisionBaker.h"
#endif
#if VXE_WITH_OZZ
#include "vxe/animation/CharacterAnimator.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace vxe;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct FrameTimer {
    double worst = 0.0, total = 0.0;
    uint64_t frames = 0;
    void add(double v) { worst = std::max(worst, v); total += v; ++frames; }
    double avg() const { return frames ? total / double(frames) : 0.0; }
};

const char* pathName(TerrainPipelinePath p) {
    switch (p) {
    case TerrainPipelinePath::MeshShader: return "mesh shaders (AS/MS) + compute near-field displacement";
    case TerrainPipelinePath::Tessellation: return "hardware tessellation (VS/HS/DS)";
    default: return "classic VS/PS";
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    std::printf("VXE demo - volumetric voxel engine pipeline (headless)\n");
    std::printf("=======================================================\n");

    // ---- Systems --------------------------------------------------------------
    TaskSystem tasks;
    TerrainParams terrainParams;
    DensityGenerator generator(terrainParams);
    DeltaStore deltas;

    StreamerConfig sc;
    sc.maxLod = quick ? 3 : 4;
    sc.viewDistance = quick ? 700.0 : 1500.0;
    ChunkStreamer streamer(tasks, generator, deltas, sc);

    NullRenderDevice device;
    TerrainRenderer renderer(device, tasks);
    MaterialSystem materials;
    materials.registerDefaultTerrainMaterials();
    streamer.addListener(&renderer);

#if VXE_WITH_JOLT
    PhysicsWorld physics(tasks);
    TerrainCollisionBaker baker(tasks, physics);
    streamer.addListener(&baker);
    physics.startThread();   // 60 Hz on its own thread; main thread never touches Jolt state
#endif

    std::printf("world      : %.1f km x %.1f km (%.0f km^2), y %d..%d m\n", world::kWorldHalfExtentXZ * 2 / 1000.0,
                world::kWorldHalfExtentXZ * 2 / 1000.0,
                double(world::kWorldHalfExtentXZ) * 2 * world::kWorldHalfExtentXZ * 2 / 1e6, world::kWorldMinY, world::kWorldMaxY);
    std::printf("chunks     : %d^3 cells, LOD0 %.0f m ... LOD%d %.0f m, view %.0f m\n", world::kChunkCells,
                world::kChunkCells * world::kVoxelSize, sc.maxLod, double(world::kChunkCells << sc.maxLod), sc.viewDistance);
    std::printf("workers    : %u (+ main%s)\n", tasks.workerCount(), VXE_WITH_JOLT ? " + physics thread" : "");
    std::printf("noise      : %s\n", generator.backendName());
    std::printf("gpu path   : %s\n\n", pathName(renderer.path()));

    // ---- Spawn on land ---------------------------------------------------------
    Vec3d spawn{2400.0, 0.0, -1800.0};
    for (int i = 0; i < 64; ++i) {
        const float h = generator.sampleHeight(float(spawn.x), float(spawn.z));
        if (h > 6.f && h < 200.f) { spawn.y = h; break; }
        spawn.x += 173.0; spawn.z += 97.0;
    }
    std::printf("[1] spawn at (%.0f, %.1f, %.0f)\n", spawn.x, spawn.y, spawn.z);

    FrameTimer mainThread;
    auto frame = [&](const Vec3d& focus) {
        const auto t0 = Clock::now();
        streamer.update(focus);
        device.beginFrame();
        renderer.update();
        materials.update(1.f / 60.f);
        device.endFrame();
        mainThread.add(ms(Clock::now() - t0));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // stand-in for render/present
    };

    // ---- 2. Initial streaming ---------------------------------------------------
    {
        const auto t0 = Clock::now();
        int frames = 0;
        do { frame(spawn); ++frames; } while (!streamer.isIdle() && frames < 200000);
        for (int i = 0; i < 8; ++i) frame(spawn);   // let uploads drain
        const auto& s = streamer.stats();
        std::printf("[2] initial stream: %.2f s, %d frames, %u chunks (%u with geometry, %u culled empty)\n",
                    ms(Clock::now() - t0) / 1000.0, frames, s.residentChunks, s.residentChunks - s.emptyChunks, s.emptyChunks);
        std::printf("    per LOD:");
        for (int l = 0; l <= sc.maxLod; ++l) std::printf(" L%d=%u", l, s.perLod[l]);
        std::printf("\n    triangles %.2f M, resident LOD0 density fields %u (%.1f MB)\n", double(s.triangles) / 1e6,
                    s.residentFields, double(s.residentFields) * sizeof(DensityField) / 1e6);
        const auto& rs = renderer.stats();
        std::printf("    GPU: %u non-empty chunks resident (%u pending), vertices %.1f MB, indices %.1f MB, meshlets %.1f MB, uploaded %.1f MB\n",
                    rs.residentChunks, rs.pendingUploads, rs.vertexBytes / 1e6, rs.indexBytes / 1e6, rs.meshletBytes / 1e6, rs.uploadedBytes / 1e6);
        std::printf("    main-thread frame cost: avg %.2f ms, worst %.2f ms (all meshing/baking is off-thread)\n",
                    mainThread.avg(), mainThread.worst);
    }
#if VXE_WITH_JOLT
    baker.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        const auto bs = baker.stats();
        std::printf("    Jolt: %u terrain bodies, %llu shapes baked (last %.2f ms on a worker)\n", bs.liveBodies,
                    (unsigned long long)bs.shapesBaked, bs.lastBakeMs);
    }
#endif

    // ---- 3. Travel at vehicle speed -----------------------------------------------
    {
        mainThread = {};
        const uint64_t builtBefore = streamer.stats().chunksBuilt;
        const double speed = 30.0;                 // m/s (~108 km/h)
        const int frames = quick ? 240 : 800;
        Vec3d focus = spawn;
        const auto t0 = Clock::now();
        for (int i = 0; i < frames; ++i) {
            focus.x += speed / 60.0;
            focus.y = generator.sampleHeight(float(focus.x), float(focus.z)) + 2.0;
            frame(focus);
        }
        std::printf("[3] drove %.0f m at %.0f m/s: %llu chunk builds, main thread avg %.2f / worst %.2f ms, %.2f s wall\n",
                    speed * frames / 60.0, speed, (unsigned long long)(streamer.stats().chunksBuilt - builtBefore),
                    mainThread.avg(), mainThread.worst, ms(Clock::now() - t0) / 1000.0);
        spawn = focus;
        int settle = 0;
        while (!streamer.isIdle() && settle < 200000) { frame(spawn); ++settle; }
    }

    // ---- 4. Terraforming --------------------------------------------------------
    Terraformer terraformer(streamer, generator, deltas);
    VoxelTerrainQuery terrainQuery(streamer, deltas, generator);
    const double groundY = spawn.y - 2.0;
    const Vec3d digAt{spawn.x + 6.0, groundY, spawn.z};
    {
        TerraformOp dig;
        dig.op = BrushOp::Subtract;
        dig.center = digAt;
        dig.halfExtents = Vec3f(4.f);
        const auto t0 = Clock::now();
        const auto res = terraformer.apply(dig);
        const double applyMs = ms(Clock::now() - t0);
        int frames = 0;
        const auto t1 = Clock::now();
        do { frame(spawn); ++frames; } while (!streamer.isIdle() && frames < 100000);
        std::printf("[4] carved r=4 m sphere: %u voxels, %u resident samples patched, apply() %.2f ms, remeshed in %d frames (%.1f ms)\n",
                    res.voxelsWritten, res.residentSamplesPatched, applyMs, frames, ms(Clock::now() - t1));
        std::printf("    density at crater centre now %.2f (air > 0)\n", terrainQuery.density(digAt));
    }
#if VXE_WITH_JOLT
    {
        baker.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // physics thread swaps shapes in its pre-step
        const BodyHandle ball = physics.createDynamicSphere({digAt.x, digAt.y + 8.0, digAt.z}, 0.4f);
        std::this_thread::sleep_for(std::chrono::milliseconds(quick ? 1500 : 2500));
        const Vec3d p = physics.bodyPosition(ball);
        std::printf("    Jolt: ball dropped into the crater came to rest at y=%.2f (rim %.2f, crater floor ~%.2f)\n", p.y,
                    groundY, digAt.y - 4.0);
        physics.removeBody(ball);
    }
#endif

    // ---- 5. Building on terrain -------------------------------------------------
    {
        BuildingSystem building(terrainQuery);
        terraformer.addListener(&building);
        const auto floorT = building.registerType({"wood_floor_2x2", {4, 1, 4}, StructuralMaterial::Wood, true});
        const auto wallT = building.registerType({"wood_wall_2x2", {4, 4, 1}, StructuralMaterial::Wood, false});
        const auto beamT = building.registerType({"wood_beam_2m", {4, 1, 1}, StructuralMaterial::Wood, false});

        const double bx = spawn.x - 20.0, bz = spawn.z + 12.0;
        // Find the surface under the plot by ray-marching the voxel field.
        const auto hit = terrainQuery.raycastDown({bx + 1.0, groundY + 40.0, bz + 1.0}, 120.f);
        const double surf = hit ? groundY + 40.0 - *hit : groundY;
        const double floorY = std::floor(surf / 0.5) * 0.5;   // snap into the grid, slightly embedded
        auto f = building.tryPlace(floorT, {bx, floorY, bz});
        auto w = building.tryPlace(wallT, {bx, floorY + 0.5, bz});
        auto b1 = building.tryPlace(beamT, {bx, floorY + 2.5, bz});         // on top of the wall
        auto b2 = building.tryPlace(beamT, {bx + 2.0, floorY + 2.5, bz});   // cantilevered out
        auto b3 = building.tryPlace(beamT, {bx + 4.0, floorY + 2.5, bz});
        std::printf("[5] built on terrain at y=%.1f: floor %s (grounded=%d, support %.0f), wall %.1f, beams %.1f -> %.1f -> %.1f\n",
                    floorY, f.ok() ? "ok" : "rejected", f.ok() ? int(building.piece(f.id)->grounded) : 0, f.support, w.support,
                    b1.support, b2.support, b3.support);

        TerraformOp undermine;
        undermine.op = BrushOp::Subtract;
        undermine.shape = BrushShape::Box;
        undermine.center = {bx + 1.0, floorY - 1.5, bz + 1.0};
        undermine.halfExtents = {3.f, 2.0f, 3.f};
        undermine.smoothK = 0.f;   // hard CSG: keeps the DC crease along the pit rim
        std::printf("    digging a 6x4x6 m pit under the foundation (policy allows edits: %s)\n",
                    building.allowsTerrainEdit({undermine.center - Vec3d(3), undermine.center + Vec3d(3)}) ? "yes" : "no");
        terraformer.apply(undermine);
        const auto collapsed = building.drainCollapses();
        std::printf("    -> %zu pieces lost support and collapsed (debris events for the physics layer)\n", collapsed.size());
        while (!streamer.isIdle()) frame(spawn);
    }

    // ---- 6. Save / reload deltas -------------------------------------------------
    {
        const auto bytes = deltas.serialize();
        { std::ofstream out("world_deltas.vxd", std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
        DeltaStore reloaded;
        const bool ok = reloaded.deserialize(bytes);
        std::printf("[6] saved world_deltas.vxd: %zu overrides in %zu chunks -> %zu bytes (%.2f B/voxel); reload %s\n",
                    deltas.overrideCount(), deltas.chunkCount(), bytes.size(), double(bytes.size()) / double(std::max<size_t>(1, deltas.overrideCount())),
                    ok && reloaded.overrideCount() == deltas.overrideCount() ? "verified" : "FAILED");
    }

#if VXE_WITH_OZZ
    // ---- 7. Character animation with terrain foot IK ---------------------------------
    {
        auto rig = CharacterAnimator::makeProceduralBiped();
        CharacterAnimator hero(rig);
        Vec3d pos{spawn.x - 10.0, 0.0, spawn.z - 10.0};
        int reachedFrames = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < 180; ++i) {
            pos.x += 1.6 / 60.0;
            const auto h = terrainQuery.raycastDown({pos.x, groundY + 40.0, pos.z}, 120.f);
            pos.y = h ? groundY + 40.0 - *h : groundY;
            hero.update(1.f / 60.f, 1.6f, pos, 0.f, &terrainQuery);
            reachedFrames += hero.leftFootReached() && hero.rightFootReached();
        }
        std::printf("[7] ozz: %d-joint biped walked 4.8 m over terrain, avg %.3f ms/update, IK targets reached %d/180 frames, pelvis offset %.2f m\n",
                    hero.jointCount(), ms(Clock::now() - t0) / 180.0, reachedFrames, hero.pelvisOffset());
    }
#endif

    // ---- 8. GPU frame build -------------------------------------------------------
    {
        Frustum fr{};
        // 90 degree frustum looking down +x (camera-relative); planes point inward.
        const float s = 0.70710678f;
        fr.planes = {{{{s, 0, s}, 0.f}, {{s, 0, -s}, 0.f}, {{s, s, 0}, 0.f}, {{s, -s, 0}, 0.f},
                      {{1, 0, 0}, -0.1f}, {{-1, 0, 0}, float(sc.viewDistance)}}};
        const auto t0 = Clock::now();
        const TerrainFrame tf = renderer.buildFrame(spawn, &fr, 48.f);
        std::printf("[8] frame build: %zu visible chunk instances, %zu mesh dispatches, %zu indexed draws, %zu near-field (compute displacement) chunks, %.3f ms\n",
                    tf.instances.size(), tf.meshDispatches.size(), tf.indexedDraws.size(), tf.nearFieldInstances.size(),
                    ms(Clock::now() - t0));
        std::printf("    NullRenderDevice: %.1f MB GPU buffers allocated, %.1f MB copied\n", device.bytesAllocated() / 1e6,
                    device.bytesCopiedTotal() / 1e6);
    }

#if VXE_WITH_JOLT
    physics.stopThread();
    baker.flush();
#endif
    renderer.flushPackJobs();
    std::printf("\ndone. tasks executed: %llu\n", (unsigned long long)tasks.tasksExecuted());
    return 0;
}
