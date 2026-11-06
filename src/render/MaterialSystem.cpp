#include "vxe/render/MaterialSystem.h"

#include <algorithm>

namespace vxe {

void MaterialSystem::registerMaterial(const MaterialDesc& desc) {
    Entry e;
    e.desc = desc;
    if (m_gpu.size() <= desc.id) m_gpu.resize(size_t(desc.id) + 1, GpuMaterial{});
    requestImages(e);
    writeGpu(e);
    m_entries[desc.id] = std::move(e);
    m_dirty = true;
}

void MaterialSystem::setImages(MaterialId id, const ImageMaps& maps) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    Entry& e = it->second;
    e.desc.images = maps;
    e.failed = false;
    // Keep current slices + blend until the new set is resident, then crossfade
    // from procedural again (cheap, and avoids holding two slice sets).
    requestImages(e);
    m_dirty = true;
}

void MaterialSystem::requestImages(Entry& e) {
    e.pending.clear();
    e.transitioning = false;
    if (!e.desc.images || !m_streamer) return;
    const ImageMaps& m = *e.desc.images;
    auto req = [&](const std::string& path, TextureKind kind) {
        if (!path.empty()) e.pending.push_back({kind, m_streamer->request(path, kind)});
    };
    req(m.albedo, TextureKind::Albedo);
    req(m.normal, TextureKind::Normal);
    req(m.orm, TextureKind::Orm);
    req(m.height, TextureKind::Height);
}

void MaterialSystem::writeGpu(const Entry& e) {
    const MaterialDesc& d = e.desc;
    GpuMaterial& g = m_gpu[d.id];
    const float keepBlend = g.imageBlend;
    g = GpuMaterial{};
    g.baseColor[0] = d.procedural.baseColor.x; g.baseColor[1] = d.procedural.baseColor.y; g.baseColor[2] = d.procedural.baseColor.z;
    g.colorVariation[0] = d.procedural.colorVariation.x; g.colorVariation[1] = d.procedural.colorVariation.y; g.colorVariation[2] = d.procedural.colorVariation.z;
    g.roughness = d.procedural.roughness;
    g.metallic = d.procedural.metallic;
    g.noiseScale = d.procedural.noiseScale;
    g.textureScale = d.textureScale;
    g.triplanarSharpness = d.triplanarSharpness;
    g.imageBlend = e.transitioning ? keepBlend : 0.f;
    g.albedoSlice = g.normalSlice = g.ormSlice = g.heightSlice = kNoTextureSlice;
    g.displacementAmplitude = d.displacementAmplitude;
    g.displacementFrequency = d.displacementFrequency;
    g.strataFrequency = d.procedural.strataFrequency;
    g.crackDensity = d.procedural.crackDensity;
    g.pattern = uint32_t(d.procedural.pattern);
    g.macroVariation = d.macroVariation;
    g.heightBlendSharpness = d.heightBlendSharpness;
    if (e.transitioning || keepBlend > 0.f) {
        for (const Pending& p : e.pending) {
            if (!p.done) continue;
            switch (p.kind) {
            case TextureKind::Albedo: g.albedoSlice = p.slice; break;
            case TextureKind::Normal: g.normalSlice = p.slice; break;
            case TextureKind::Orm: g.ormSlice = p.slice; break;
            case TextureKind::Height: g.heightSlice = p.slice; break;
            }
        }
    }
}

void MaterialSystem::update(float dt) {
    for (auto& [id, e] : m_entries) {
        if (e.pending.empty() || e.failed) continue;
        GpuMaterial& g = m_gpu[id];

        if (!e.transitioning) {
            bool allDone = true;
            for (Pending& p : e.pending) {
                if (p.done) continue;
                const auto r = m_streamer->poll(p.request);
                if (r.status == ITextureStreamer::Status::Resident) { p.done = true; p.slice = r.slice; }
                else if (r.status == ITextureStreamer::Status::Failed) { e.failed = true; allDone = false; break; }
                else allDone = false;
            }
            if (e.failed) {            // stay procedural, permanently for this request set
                g.imageBlend = 0.f;
                m_dirty = true;
                continue;
            }
            if (allDone) {
                e.transitioning = true;
                g.imageBlend = 0.f;
                writeGpu(e);           // publish slices with blend 0, then ramp
                m_dirty = true;
            }
            continue;
        }
        if (g.imageBlend < 1.f) {
            g.imageBlend = std::min(1.f, g.imageBlend + dt / std::max(1e-3f, e.desc.transitionSeconds));
            m_dirty = true;
        }
    }
}

void MaterialSystem::registerDefaultTerrainMaterials() {
    auto make = [](const char* name, MaterialId id, Vec3f color, ProceduralPattern pat, float rough, float strata,
                   float cracks, float disp) {
        MaterialDesc d;
        d.name = name;
        d.id = id;
        d.procedural.baseColor = color;
        d.procedural.pattern = pat;
        d.procedural.roughness = rough;
        d.procedural.strataFrequency = strata;
        d.procedural.crackDensity = cracks;
        d.displacementAmplitude = disp;
        return d;
    };
    registerMaterial(make("air", kMatAir, {0, 0, 0}, ProceduralPattern::Soil, 1.f, 0.f, 0.f, 0.f));
    registerMaterial(make("grass", kMatGrass, {0.24f, 0.36f, 0.14f}, ProceduralPattern::Grass, 0.9f, 0.f, 0.f, 0.04f));
    registerMaterial(make("dirt", kMatDirt, {0.33f, 0.24f, 0.16f}, ProceduralPattern::Soil, 0.95f, 0.f, 0.05f, 0.06f));
    registerMaterial(make("rock", kMatRock, {0.42f, 0.40f, 0.38f}, ProceduralPattern::Rock, 0.8f, 0.6f, 0.6f, 0.22f));
    registerMaterial(make("sand", kMatSand, {0.76f, 0.68f, 0.50f}, ProceduralPattern::Sand, 0.92f, 0.f, 0.f, 0.03f));
    registerMaterial(make("snow", kMatSnow, {0.92f, 0.94f, 0.97f}, ProceduralPattern::Snow, 0.55f, 0.f, 0.f, 0.05f));
    registerMaterial(make("deepstone", kMatDeepstone, {0.22f, 0.22f, 0.25f}, ProceduralPattern::Rock, 0.7f, 1.2f, 0.8f, 0.25f));
    registerMaterial(make("gravel", kMatGravel, {0.45f, 0.43f, 0.40f}, ProceduralPattern::Rock, 0.9f, 0.f, 0.f, 0.08f));
}

} // namespace vxe
