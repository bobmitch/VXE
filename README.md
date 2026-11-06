# VXE — Volumetric Voxel & Tessellation Engine (core scaffolding)

Engine core for a ~512 km² survival-crafting world with fully deformable volumetric terrain, caves, and grid-based base building. Modern C++20, CMake, no general-purpose engine underneath. Third-party code is limited to the four libraries named in the brief, pinned and fetched automatically:

| Library | Version | Used for |
|---|---|---|
| Jolt Physics | v5.6.0 (double precision) | Terrain collision (async `MeshShape` baking), queries, dynamics |
| FastNoise2 | v1.1.1 (strict FP) | SIMD density evaluation |
| meshoptimizer | v1.3 | Cache/overdraw/fetch optimisation, meshlets, border-locked collision LOD |
| ozz-animation | 0.16.0 | Sampling, blending, two-bone foot IK |

**Status:** everything below compiles, and the engine library, tests and headless demo run on Linux/GCC 13. All 10 shader entry points compile with DXC to both DXIL (SM 6.5) and SPIR-V (Vulkan 1.3) with warnings-as-errors. The one deliberate gap is the GPU backend. The RHI interface, GPU data layouts, allocators, upload ring, draw-list builder and shaders are implemented, but the Vulkan / D3D12 device implementations are not. A CPU `NullRenderDevice` stands in for them (see [Known limitations](#known-limitations)).

---

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release      # fetches the 4 libraries from GitHub
cmake --build build -j
./build/vxe_tests                                    # 13 tests / 67 checks
./build/vxe_demo                                     # end-to-end headless run (add --quick for a shorter one)
./build/vxe_viewer                                   # windowed interactive demo (Windows / D3D11)
DXC=/path/to/dxc ./shaders/compile_shaders.sh        # DXIL + SPIR-V for every entry point
```

Requirements: CMake ≥ 3.24, a C++20 compiler (GCC 12+, Clang 15+, MSVC 19.34+), and Git (for FetchContent). For offline builds, point FetchContent at local checkouts with `-DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=…`, `…_FASTNOISE2`, `…_MESHOPTIMIZER`, `…_OZZ`. Each library can be switched off (`-DVXE_WITH_JOLT=OFF`, etc.). The engine falls back to a scalar noise implementation, a naive meshlet builder, and no physics or animation.

---

## Architecture

```
                         MAIN THREAD                     TASK SYSTEM WORKERS (priority DAG)            PHYSICS THREAD (60 Hz)
                         -----------                     ----------------------------------            ----------------------
 player / vehicle  ──►  ChunkStreamer::update(focus)
 (double precision)       │ implicit-octree LOD select
                          │ coverage-checked retirement
                          │ budgeted dispatch ──────────►  ┌──────────────────────────────┐
                          │                                │ 1 generate                   │  FastNoise2 SIMD grids
                          │                                │   2D columns (38²) + 3D      │  continental ▸ ridged ▸ hills
                          │                                │   overhang/caves (38³)       │  overhang ▸ spaghetti ▸ cheese
                          │                                │   + DeltaStore::applyTo()    │  (shared lock)
                          │                                └──────────────┬───────────────┘
                          │                                ┌──────────────▼───────────────┐
                          │                                │ 2 Dual Contouring            │  ENO Hermite normals, QEF/SVD,
                          │                                │   (sharpness → Surface Nets) │  owned-edge seam rule
                          │                                └──────────────┬───────────────┘
                          │                                ┌──────────────▼───────────────┐
                          │                                │ 3 meshoptimizer              │  vcache ▸ overdraw ▸ fetch
                          │                                │                              │  meshlets 64v/124t + bounds
                          │                                │                              │  simplify(LockBorder) → collision
                          │                                └──────────────┬───────────────┘
                          │  ◄──── ConcurrentQueue<BuildResult> ──────────┘
                          │ integrate (version check), keep LOD0 field resident
                          ├──► TerrainRenderer  ──────────►  4 pack/quantise (16 B verts) ──┐
                          │     update(): OffsetAllocator mega-buffers, UploadRing (fenced)  │
                          │     buildFrame(camera): camera-relative instances,               │
                          │     DispatchMesh / DrawIndexed indirect args  ◄──────────────────┘
                          │                    │
                          │                    ▼  GPU:  AS (frustum+cone cull) ─► MS ─────────────┐
                          │                           or VS ─► HS (per-edge distance factors) ─► DS ─┤─► PS: triplanar PBR,
                          │                           + CS near-field displacement (MS path) ───────┘   procedural ⇄ image blend
                          │
                          └──► TerrainCollisionBaker ──►  5 JPH::MeshShapeSettings::Create()
                                 latest[key] = version       (BVH build on a worker)
                                                              push {key, version, ShapeRefC} ──────►  pre-step hook:
                                                                                                         drop stale versions
                                                                                                         SetShape / batch AddBodies
                                                                                                         ActivateBodiesInAABox
                                                                                                       PhysicsSystem::Update
                                                                                                         (Jolt jobs → same TaskSystem
                                                                                                          via JoltJobSystemAdapter)

  Terraformer::apply(op)  (main, O(brush))                    BuildingSystem (main)
    read ▸ smooth CSG ▸ quantise ▸ DeltaStore (1 lock)          grid pieces ⟂ voxels; anchors via ITerrainQuery
    patch every resident field incl. neighbours' padding        widest-path support solve (StructureGraph)
    invalidateRegion() → Critical remesh → GPU + Jolt           onTerrainEdited → re-anchor → collapse events
```

### Threading contract

* **Main thread** owns streaming state, resident density fields, the delta store writer, building and GPU residency. Its per-frame cost is selection and integration only.
* **Workers** run pure functions of their inputs: generate, contour, optimise, pack, bake. Results return through lock-protected queues. Every result carries a globally unique version, so superseded or retired work is dropped instead of applied.
* **The physics thread** is the only writer of Jolt state. Shapes arrive pre-built and are applied in its pre-step hook. Jolt's own jobs run on the shared `TaskSystem` at `Critical` priority, so there is no second thread pool.
* **Queries** (`PhysicsWorld::castRay`) take a shared lock that excludes only the `Update()` window, because Jolt forbids queries during an update.

### Memory strategy

| Pool | What | Notes |
|---|---|---|
| `BlockPool<DensityField>` | 38³ samples × (float + u8) = 268 KB | Hard streaming budget (`maxDensityFields`). Acquire fails → back-pressure. LOD0 fields stay resident (edits, queries); uniform LOD0 fields and all LOD > 0 fields are released after contouring. |
| `thread_local` scratch | ~0.9 MB per worker (noise grids, cell-vertex map, gradients) | Allocated once per thread. Zero steady-state allocation in the hot path. |
| GPU mega-buffers + `OffsetAllocator` | vertices, indices, meshlets, meshlet streams, bounds | No per-chunk API objects. Freed ranges are recycled only after `completedFrame()` passes the frame that used them (`DeferredFrees`). |
| `UploadRing` | persistently mapped staging ring | Frame-fenced, wrap-aware, per-frame byte budget. |
| `DeltaStore` | sparse → dense per-chunk overrides | ~5 B/entry sparse. Switches to bitmask + arrays above 6000 entries. |

### World scale and precision

The world is 707 × 32 m chunks per side: 22.6 km × 22.6 km ≈ 512 km², with y from −512 to 1536 m. A float at 11 km has ~1 mm resolution, so absolute float coordinates are never used where jitter would show:

* Authoritative positions are `double`. Terrain addressing uses `int32` voxel and chunk keys. Mesh vertices are chunk-local floats (≤ 34 m).
* GPU positions are camera-relative. `chunkOrigin − cameraPos` is computed in double on the CPU once per chunk per frame. Vertex shaders only ever see small floats, so no origin rebasing is needed.
* Shader noise and triplanar UVs use `noiseOrigin = chunkOrigin mod 4096`, and all shader noise is periodic at 4096 m. This keeps sub-millimetre precision anywhere in the world without a visible wrap.
* Jolt is built with `JPH_DOUBLE_PRECISION`. Bodies sit at double-precision chunk origins with float shapes.
* `FloatingOrigin` exists for middleware that insists on float world space. It publishes rebase events.

---

## Subsystem A — streaming, density, isosurface

**Selection** (`ChunkStreamer`): an implicit octree. Top-level LOD `maxLod` chunks within `viewDistance` split into 8 children while `distance < splitFactor × size`. The leaves tile space with no overlap. Two kinds of leaf are marked ready with no work at all:

* chunks above a conservative column envelope (`columnRange`, batched FastNoise2 probes with a margin), and
* LOD > 0 chunks deep below it.

A leaf that has player edits nearby is never culled. `DeltaStore::hasEditsNear` makes this an O(27) check. A chunk that leaves the desired set is retired only once every desired leaf overlapping it has a mesh. This is a coverage walk over the desired-node tree, with a timeout. The result is no holes while travelling.

**Density** (`DensityGenerator`) runs a column-first pipeline:

1. Five 2D FastNoise2 grids (38² points): continental fBm, mountain mask, ridged multifractal, hills, cave entrances. These shape a height via a smooth continental spline.
2. 3D grids (38³), only where needed: an overhang fBm in the surface band, and two simplex fields (spaghetti tunnels, `√(a²+b²) − r`) plus a cheese fBm (caverns) for LOD ≤ 1 underground.
3. `density = clamp(max((y − h) − overhang, −caveSdf), ±8 m)`. Caves keep a rock shell under the surface except where the entrance noise opens it, and stop at a bedrock floor.

Positions are integer metres (`offset + i·2^lod`, exact in float), and FastNoise2 is built with `FASTNOISE2_STRICT_FP`. Neighbouring chunks therefore produce bit-identical samples on shared points. The skip-3D early-outs are exact for the same reason: caves only carve below the 2D surface.

**Isosurface** (`contourChunk`): Dual Contouring.

* **Vertex placement.** One vertex per sign-changing cell. It is placed by a QEF minimised through Jacobi SVD with eigenvalue truncation, relative to the mass point, and clamped to the cell.
* **Normals.** Hermite normals come from ENO (essentially non-oscillatory) differences. These pick the one-sided stencil that doesn't straddle a crease, which is what preserves sharp features in sampled data. `sharpness` blends toward Surface Nets: 1.0 near the player, 0.35 for far LODs.
* **Seams.** The mesher visits cells `[0, N]` and emits quads only for owned edges: along-axis coordinate in `[0, N−1]`, perpendicular coordinates in `[1, N]`. Each global edge is emitted exactly once. The padded field (2 samples each side) gives both chunks identical ENO stencils on shared cells, so boundary vertices match bit-for-bit. The test `chunk_seams_are_watertight` checks this on real terrain.

## Subsystem B — terraforming, persistence, physics sync

* **`Terraformer::apply`** runs on the main thread and costs O(brush volume).
  * Brushes are sphere, box and cylinder SDFs. Operations are subtract, add, flatten (the Valheim hoe), smooth and paint.
  * Smooth-CSG with radius `smoothK` is used. `smoothK = 0` keeps hard edges, which DC then preserves.
  * New values are written to the `DeltaStore` under one exclusive lock. The *dequantised* values are written back into every resident field whose padded range contains the voxel, including neighbours' padding rings. The edited region is then invalidated at all LODs with `Critical` priority.
  * LOD0 chunks re-contour from a snapshot of their resident field and skip regeneration. Coarser LODs regenerate and pick up the override on their lattice.
  * Races are handled: an edit that lands while a generation is in flight forces a regenerate-with-deltas on completion.
* **`DeltaStore`** (sparse density override arrays):
  * Overrides are stored per owning LOD0 chunk, keyed by a 16-bit local index.
  * Density is quantised to int16 over ±8 m, a 0.24 mm step.
  * The on-disk format is delta- and varint-coded `(indexΔ<<1 | materialChanged)`, plus zigzag `qΔ`, plus an optional material byte, with an FNV-1a checksum.
  * Measured: **2.1 B/voxel** on a smooth tunnel (raw would be 5 B) and 3.1 B/voxel on a small mixed edit.
  * Reload produces identical meshes; there is a test for this.
* **Jolt** (`TerrainCollisionBaker` + `PhysicsWorld`): the timeline is shown in the diagram. In summary:
  * The render thread never touches Jolt.
  * The main thread only records `latest[key] = version` and submits a bake task.
  * Workers build the `MeshShape`. The collision mesh is meshoptimizer-simplified to about 50% with `LockBorder`, so neighbouring chunks stay sealed.
  * The physics thread applies shapes in its pre-step hook, dropping any stale versions. New bodies are batch-added with `AddBodiesPrepare/Finalize`. Changed terrain wakes dynamic bodies in its box, so crates fall into fresh holes.

## Subsystem C — shaders, displacement, materials

The shaders live in `shaders/` and are HLSL 2021, also valid Slang. They compile to DXIL and SPIR-V via `compile_shaders.sh`.

| File | Stage(s) | Role |
|---|---|---|
| `Common.hlsli` | — | Byte-exact mirrors of the C++ layouts, bindings, 16 B vertex decode (unorm16 pos, octahedral normal) |
| `Noise.hlsli` | — | Periodic gradient noise **with analytic derivatives**, fBm, cellular cracks, strata |
| `Triplanar.hlsli` | — | Sign-corrected triplanar UVs, Whiteout normal blending, analytic UV gradients |
| `Displacement.hlsli` | — | **Single definition** of micro-relief (fBm + cracks + strata + image height), used by both DS and CS |
| `TerrainSurface.hlsli` | — | Procedural ⇄ image material eval, height blending, GGX/Smith/Schlick |
| `TerrainMesh.as/.ms.hlsl` | AS 6.5 / MS 6.5 | Meshlet frustum and normal-cone culling (wave-size independent), vertex pulling |
| `TerrainTess.hlsl` | VS/HS/DS 6.1 | Per-edge screen-space tessellation factors (crack-free), patch frustum culling, displacement |
| `TerrainDisplace.cs.hlsl` | CS 6.5 | Near-field compute subdivision and displacement for the mesh-shader path, plus an indirect-args writer |
| `TerrainNearField.hlsl` | VS 6.1 | Vertex pulling for the CS stream, and the classic-path VS |
| `Terrain.ps.hlsl` | PS 6.1 | `SV_Barycentrics` + `GetAttributeAtVertex` per-pixel material weights, top-2 height blend |

Key decisions:

* **Triplanar on 360° surfaces.**
  * UVs are mirrored by the sign of the normal, so cave ceilings and overhang undersides are never texture-mirrored.
  * Normal maps use the Whiteout blend in world space.
  * UV gradients are derived analytically from `ddx/ddy(noisePos)`, taken once in uniform control flow. Per-pixel material ids are divergent, so calling `ddx` inside the material branches would be undefined.
  * Procedural detail uses 3D noise directly, which needs no projection.
* **Distance-based displacement.**
  * Hardware path: tessellation factors are computed per *edge* from that edge's midpoint and projected length. Adjacent patches therefore always agree, giving `fractional_odd` crack-free output.
  * Mesh-shader path: hardware tessellation isn't available there, so a compute pass uniformly subdivides near-field LOD0 triangles and displaces each lattice point once.
  * In both paths, displacement is barycentric-blended across corner materials, which keeps it continuous across patches. It fades to zero at `displacementFarDistance`, so the edge against undisplaced geometry stays closed.
* **Procedural ⇄ image materials** (`MaterialSystem`). Every material is always renderable procedurally. Image maps are an upgrade layered on top:
  * Asset pipeline: `.vxmat` source, then a cooker producing BC7 albedo, BC5 normal, BC7 ORM and BC4 height with full mips, grouped into Texture2DArrays, then streamed in by `ITextureStreamer`.
  * Until *all* declared maps are resident, `imageBlend = 0`. It then ramps 0 → 1 over `transitionSeconds`.
  * Failures keep the material procedural.
  * `macroVariation` keeps a procedural low-frequency tint over images to break up tiling.
  * Displacement switches from procedural height to image height through the same `imageBlend`, so geometry and shading never disagree.

## Subsystem D — decoupled grid building

Structures and terrain are kept apart by construction:

1. Structures never write density, and the voxel field never stores structures.
2. Pieces live on a world-aligned 0.5 m build grid, independent of the 1 m voxel lattice and of chunk boundaries.
3. Terrain affects structures only through `ITerrainQuery`:
   * anchoring: the fraction of base-footprint probes that are solid within tolerance, and
   * placement validation: whether the piece is mostly buried.

   `VoxelTerrainQuery` reads resident LOD0 density trilinearly, so it is exact the instant an edit lands. A Jolt raycast is available through `PhysicsWorld::castRay` for anything that must also see props.
4. Terrain edits notify `BuildingSystem`, which re-anchors pieces near the edit and re-solves their components. Pieces that lose support become `CollapseEvent`s, which the game turns into debris bodies. A `ProtectFoundations` policy instead lets tools refuse edits under anchored pieces.

**Stability** (`StructureGraph`): support flows across contacts as `s(q) = max_p min(max_q, s(p) − loss(q, dir)·max_q)`, with separate vertical and horizontal losses per material. Wood, core wood, stone and iron are defined; stone has 100% horizontal loss, so it cannot cantilever. Support is non-increasing along any path, so the fixed point is a widest-path problem. One max-heap Dijkstra per affected component gives the exact result in O(E log V), and broken pieces are excluded from transmitting, so one pass is final.

## Animation (ozz)

`CharacterAnimator` runs the following steps, with no allocations after construction:

1. Phase-synchronised `SamplingJob`s over locomotion clips.
2. A 1D blend space by ground speed (`BlendingJob`).
3. `LocalToModelJob`.
4. **Ground-relative foot IK.** The authored swing is kept, and each ankle is offset by how far the terrain under it deviates from the root's floor. The pelvis is lowered by the largest drop. `IKTwoBoneJob` runs per leg, and its corrections are folded back into the SoA local pose.
5. A skinning palette.

It loads cooked `.ozz` archives, or builds a procedural biped used for tests and as a placeholder.

---

## Measured (this sandbox: 2 vCPUs → 1 worker + main + physics thread, Release)

From `./build/vxe_demo` (view distance 1.5 km, LOD0–4):

| Stage | Result |
|---|---|
| Initial stream | 2312 chunks selected (1032 culled empty without work), 2.58 M triangles, in 5.4 s on **one** worker |
| Main-thread frame cost | avg 0.2–0.6 ms. Worst ~30 ms on the first selection and ~11 ms on reselection spikes while driving (see limitations) |
| Drive 400 m at 30 m/s | ~300 chunk builds streamed in, no blocking |
| Carve r = 4 m sphere | 643 voxels, `apply()` 0.3 ms, remeshed + GPU-queued in ~17 frames |
| Physics | 350 terrain bodies. A ball dropped into the fresh crater comes to rest on the crater floor |
| Building | floor grounded (100), wall 87.5, beams 75 → 55 → 35. Digging a pit under it collapses all 5 pieces |
| Deltas | 1002 overrides → 3078 bytes. Reload verified |
| GPU frame build | 231 visible instances → 231 mesh dispatches + 10 near-field compute chunks in 0.2–0.4 ms |

Per-chunk generate + contour is ~1.6 ms on one core for surface chunks.

---

## Border skirts

A chunk is meshed in isolation, so its mesh has exactly one open boundary: the chunk perimeter. Every interior edge is shared by two triangles, quad diagonals included, so finding that boundary needs no neighbour information at all — just the edges a single triangle touches. `contourChunk` hangs a collar off each of them, back along `-normal`, and the result is geometry that is buried under the neighbour's surface wherever the two chunks agree and plugs the gap wherever they do not. Both sides carry one, which is what makes it symmetric: whichever chunk's rim ends up higher is the one whose collar reaches down across the gap.

Two things make it behave:

* **Depth is measured, not guessed.** `MesherSettings::skirtDepthCells` defaults to 4.5 cells, which scales with the chunk's own stride — 4.5 m at LOD0, 144 m at LOD5. `lod_seam_cracks_are_covered_by_skirts` measures the real gap distribution across sampled LOD0|LOD1 seams and asserts the shipped default still covers p99 from both directions, so changing the constant without re-checking fails the test.
* **The collar is clipped to the solid.** Each vertex walks back along `-normal` only as far as the density field stays negative. Without that, a collar punches straight through anything thinner than its depth — a cave roof is often two metres of rock — and the part that comes out the far side hangs over the terrain, shaded by the normal of the ceiling it grew from, i.e. a large dark wing. With it, no collar vertex is ever outside the solid, which is the invariant that keeps the whole thing invisible. The test asserts that too.

Cost is proportional to the surface *perimeter*, not its area: about 9% more triangles for a chunk with a full surface, ~23% across a whole view where many far chunks are small and their perimeter is a larger fraction of them. Measured in `vxe_viewer` from a fixed viewpoint, 1280x720, vsync off, with and without `--no-skirts`: 2.76–2.92 ms/frame becomes 3.20–3.22 ms, so +14% frame time for +23% triangles. Skirts are kept out of the collision mesh — `ChunkMesh::skirtIndexStart` marks where they start — because letting them reach `meshopt_simplify` would pin the collar's lower rim as the locked border instead of the chunk boundary and unseal the join between neighbouring collision meshes.

The obvious next optimisation is to skirt only the faces whose neighbour is at a different LOD. The streamer knows those LODs at dispatch time, but acting on it means re-meshing a chunk when a *neighbour's* LOD changes, which is a cross-chunk dependency the build graph does not have today.

`vxe_viewer --no-skirts` renders the bare isosurface, for comparison.

---

## Interactive viewer (`vxe_viewer`)

`vxe_demo` runs the whole pipeline headless against `NullRenderDevice`. `vxe_viewer` puts a window in front of it so the terrain can actually be looked at and flown through. It is Windows-only and carries its own Direct3D 11 backend (one file, ~1,250 lines including the HLSL), because the engine does not ship a real device yet (see *Known limitations*).

It is deliberately the **Classic** path — plain VS/PS, one vertex/index buffer per chunk, no tessellation and no meshlets. It bypasses `TerrainRenderer`'s mega-buffer residency and consumes `OptimizedChunkMesh` straight off the `IChunkListener` callback, so `TerrainVertex` uploads with no repacking. What it does exercise is the part that matters for a reference implementation: double-precision focus point into `ChunkStreamer`, camera-relative positions narrowed to float per draw, the engine's own `vxe::Frustum` for culling, `MaterialSystem`'s GPU table for the palette, and `Terraformer` for live edits.

```bash
cmake --build build --config Release --target vxe_viewer
./build/Release/vxe_viewer                       # starts in the cinematic flyby
./build/Release/vxe_viewer --view 3000 --maxlod 5 --seed 42
./build/Release/vxe_viewer --free --lod-tint     # free camera, LOD cascade colours
```

| | |
|---|---|
| `F` | toggle cinematic flyby / free camera (any WASD or mouse input also drops to free) |
| click or `Tab` | capture the mouse for free look; `Tab` or `Esc` releases it |
| `W A S D`, `Q`/`E` | move, down/up. `Shift` ×8, `Ctrl` ×⅕, mouse wheel sets the base speed |
| LMB / RMB | dig / fill a 5 m sphere under the crosshair — real CSG, remesh and re-upload |
| `G` / `L` / `C` / `R` | wireframe, LOD tint, backface culling, respawn |
| `[` / `]` | flyby speed |

Flags: `--view`, `--maxlod`, `--seed`, `--width`, `--height`, `--novsync`, `--free`, `--cam X Y Z YAW PITCH` (a fixed, reproducible viewpoint), `--lod-tint`, `--wireframe`, `--no-skirts` / `--skirt CELLS`, and `--screenshot FILE.png --screenshot-at SECONDS --exit-after SECONDS` for capturing stills from a script.

The flyby is a closed Catmull-Rom loop laid over the terrain at startup, re-parameterised by arc length so speed stays constant however the waypoints are spaced, with a second spline for the look-at target so the camera leads into its turns. Depth is reverse-Z on a `D32_FLOAT` target with an infinite far plane, which is what makes a 0.1 m near plane usable across a multi-kilometre view distance.

Two things it makes very visible: the inter-LOD cracks listed under *Known limitations* (press `L`), and the fact that nothing here filters the procedural detail noise, which is why it fades out past ~260 m instead of aliasing.

---

## Repository layout

```
CMakeLists.txt, cmake/Dependencies.cmake     pinned FetchContent deps + options
include/vxe/core/        Math, WorldCoords (double/int addressing, FloatingOrigin), TaskSystem, Memory (pools, queues, SPSC ring)
include/vxe/world/       DensityField, DensityGenerator, ChunkStreamer
include/vxe/meshing/     ChunkMesh (+GPU meshlet layouts), DualContouring, MeshOptimizerStage
include/vxe/terraform/   DeltaStore, Terraformer
include/vxe/physics/     PhysicsWorld, JoltJobSystemAdapter, TerrainCollisionBaker
include/vxe/render/      RenderDevice (RHI + NullRenderDevice), GpuAllocators, TerrainRenderer, MaterialSystem
include/vxe/building/    StructureGraph, BuildingSystem, TerrainQuery
include/vxe/animation/   CharacterAnimator
src/...                  implementations (same structure)
shaders/                 HLSL/Slang + compile_shaders.sh
apps/vxe_demo/           headless end-to-end pipeline run
apps/vxe_viewer/         windowed interactive demo + a reference D3D11 Classic-path backend
tests/                   framework-free unit + integration tests
```

---

## Known limitations

These are stated plainly so nobody builds on an assumption that isn't true.

* **No Vulkan/D3D12 device implementation yet.** `apps/vxe_viewer` contains a self-contained D3D11 backend, but it only covers the Classic path and does not implement `IRenderDevice` — it draws `OptimizedChunkMesh` per chunk rather than going through `TerrainRenderer`'s mega-buffers. `IRenderDevice` itself covers buffers, mapping, copies and frame fences. Real backends still need:
  * pipeline creation for the three paths,
  * descriptor heaps or descriptor buffers matching the `register(tN, space0)` layout in `Common.hlsli`,
  * command signatures: `ExecuteIndirect` with a root constant for the mesh path; on Vulkan, `vkCmdDrawMeshTasksIndirectEXT` with the draw index,
  * the identity per-instance stream for `INSTANCE_INDEX`, and
  * `VK_KHR_fragment_shader_barycentric` for the PS on Vulkan.
* **LOD transitions are covered by skirts, not stitched.** Same-LOD seams are watertight by construction: both chunks visit the shared cell column, compute it from identical samples, and own disjoint halves of the edges between them. Across a LOD change there is no such agreement — four fine cells face one coarse cell, so there is no edge both sides emit a quad for, and the two surfaces simply end near each other. Measured on generator terrain, a rim sits up to 4.0 m from the other side's surface seen from the fine chunk and 6.9 m seen from the coarse one (p50 is 1.3 m / 0.7 m). The mesher now hangs a **border skirt** off the chunk perimeter to cover that; see *Border skirts* below. It hides the crack rather than closing it, so the long-term fix is still Transvoxel-style transition cells on the finer side (DC variant), which would make the join genuinely watertight and cost no extra triangles.
* **A coarse LOD can miss a feature outright.** Cave tunnels are ~4 m across and `maxCaveLod` is 1, so at a 2 m stride a tunnel is badly undersampled and its mouth can be present on the LOD0 side of a seam and absent on the LOD1 side. That is a density-LOD mismatch, not a seam crack: no skirt depth that is sane for the rest of the seam would hide it, and the fix is in the generator's LOD policy, not the mesher.
* **DC sharpness is bounded by sampled data.** Hermite data comes from linear edge interpolation of samples, so a crease *inside* an edge is approximated. Measured on a box SDF: mean surface error 0.007 m for DC versus 0.027 m for Surface Nets, but corners are about 0.35 m rounded. Exact Hermite data from analytic brush SDFs would remove this for edits.
* **Selection runs on the main thread.** Reselection happens after 4 m of travel and walks the desired octree, which causes the ~10 ms spikes measured on a 2-core box under contention. The fix is to amortise it across frames or move it to a worker with a double-buffered desired set.
* **Structure collision is left to the game layer.** One static Jolt box or compound per building is intended; it is not created here. Debris spawning from `CollapseEvent` is likewise game-side.
* **Lifetime contract.** `TerrainCollisionBaker` must be destroyed after the physics thread is stopped, because it registers a pre-step hook. The demo shows the shutdown order.
* **Deltas are one blob.** Region-file packing (e.g. 16³ chunks per file) and async I/O are not implemented. The per-chunk encoding is already region-friendly.
* **Windows/MSVC is now exercised, Linux/GCC remains the reference.** The whole project (engine, `vxe_tests` 67/67, `vxe_demo`, `vxe_viewer`) builds and runs with MSVC 19.44 / Visual Studio 2022 on Windows 11 x64, with CMake 4.3 and all four dependencies fetched. Two things were needed to get there and are now in the build files: Jolt and ozz both default to the static MSVC runtime, which collides with the engine's `/MD` at link time (`Dependencies.cmake` forces them to `/MD`), and `vxe_demo` used the `VXE_WITH_*` macros in C++ expressions, which only compiles when the matching option is ON. No other platform-specific issues surfaced.
