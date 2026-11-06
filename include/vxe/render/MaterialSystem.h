#pragma once
// -----------------------------------------------------------------------------
// MaterialSystem: terrain PBR material table with procedural <-> image sources.
//
// Every terrain material (one per voxel MaterialId) is *always* renderable
// procedurally: base colour + variation, 3D-noise micro detail, strata and
// cracks are evaluated directly in world space in the shader (3D noise needs
// no triplanar projection). Image maps are an optional upgrade layered on top:
//
//   authoring (.vxmat)  ->  cooker: BC7 albedo, BC5 normal, BC7 ORM, BC4 height,
//                           full mips, grouped by resolution into Texture2DArrays
//                       ->  runtime: ITextureStreamer streams slices in
//
// Transition rules (no visual pops):
//   * Until every declared map of a material is resident, imageBlend = 0
//     (pure procedural). Then imageBlend ramps 0 -> 1 over transitionSeconds.
//   * Missing/failed maps never break a material: it stays procedural.
//   * macroVariation keeps a low-frequency procedural tint on top of images to
//     kill visible tiling at distance.
//   * Displacement height uses the image height map when resident, otherwise
//     the procedural height (same amplitude), so geometry and shading agree.
//
// The GPU table is a StructuredBuffer<GpuMaterial> indexed by MaterialId.
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"
#include "vxe/world/DensityField.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vxe {

enum class ProceduralPattern : uint32_t { Rock = 0, Soil = 1, Grass = 2, Sand = 3, Snow = 4 };
enum class TextureKind : uint8_t { Albedo, Normal, Orm, Height };

struct ProceduralParams {
    Vec3f baseColor{0.5f, 0.5f, 0.5f};
    Vec3f colorVariation{0.08f, 0.08f, 0.08f};
    float roughness = 0.85f;
    float metallic = 0.f;
    float noiseScale = 0.35f;        // cycles per metre of the micro-detail fBm
    float strataFrequency = 0.f;     // rock layering, cycles per metre (0 = off)
    float crackDensity = 0.f;        // cellular crack mask strength
    ProceduralPattern pattern = ProceduralPattern::Rock;
};

struct ImageMaps {
    std::string albedo, normal, orm, height;   // cooked asset paths; empty = not provided
};

struct MaterialDesc {
    std::string name;
    MaterialId id = 0;
    ProceduralParams procedural;
    std::optional<ImageMaps> images;
    float textureScale = 2.0f;           // metres per texture tile (image + triplanar)
    float triplanarSharpness = 6.0f;     // blend exponent; higher = crisper axis transitions
    float macroVariation = 0.35f;        // procedural tint strength over image albedo
    float heightBlendSharpness = 8.0f;   // material-to-material height blend contrast
    float displacementAmplitude = 0.12f; // metres
    float displacementFrequency = 1.5f;  // cycles per metre for procedural height
    float transitionSeconds = 0.35f;
};

// Layout-compatible with shaders/Common.hlsli `TerrainMaterial` (96 bytes).
struct GpuMaterial {
    float baseColor[3];       float roughness;
    float colorVariation[3];  float metallic;
    float noiseScale;         float textureScale;   float triplanarSharpness; float imageBlend;
    uint32_t albedoSlice;     uint32_t normalSlice; uint32_t ormSlice;        uint32_t heightSlice;
    float displacementAmplitude; float displacementFrequency; float strataFrequency; float crackDensity;
    uint32_t pattern;         float macroVariation; float heightBlendSharpness; float pad;
};
static_assert(sizeof(GpuMaterial) == 96);

inline constexpr uint32_t kNoTextureSlice = 0xFFFFFFFFu;

// Asset-pipeline hook: the texture streamer owns texture arrays and residency.
class ITextureStreamer {
public:
    enum class Status { Pending, Resident, Failed };
    struct Poll { Status status; uint32_t slice; };
    virtual ~ITextureStreamer() = default;
    virtual uint64_t request(const std::string& path, TextureKind kind) = 0;
    virtual Poll poll(uint64_t requestId) = 0;
};

class MaterialSystem {
public:
    explicit MaterialSystem(ITextureStreamer* streamer = nullptr) : m_streamer(streamer) {}

    void registerMaterial(const MaterialDesc& desc);
    void registerDefaultTerrainMaterials();
    // Artists dropped maps on an existing material (hot reload / late DLC). Crossfades in.
    void setImages(MaterialId id, const ImageMaps& maps);

    void update(float dtSeconds);

    const std::vector<GpuMaterial>& gpuTable() const { return m_gpu; }
    bool consumeDirty() { const bool d = m_dirty; m_dirty = false; return d; }
    float imageBlend(MaterialId id) const { return id < m_gpu.size() ? m_gpu[id].imageBlend : 0.f; }

private:
    struct Pending { TextureKind kind; uint64_t request; bool done = false; uint32_t slice = kNoTextureSlice; };
    struct Entry {
        MaterialDesc desc;
        std::vector<Pending> pending;
        bool transitioning = false;
        bool failed = false;
    };
    void requestImages(Entry& e);
    void writeGpu(const Entry& e);

    ITextureStreamer* m_streamer;
    std::unordered_map<MaterialId, Entry> m_entries;
    std::vector<GpuMaterial> m_gpu;
    bool m_dirty = true;
};

} // namespace vxe
