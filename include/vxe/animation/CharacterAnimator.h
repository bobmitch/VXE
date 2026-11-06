#pragma once
// -----------------------------------------------------------------------------
// CharacterAnimator: ozz-animation runtime for survival characters.
//
// Per update (all SoA/SIMD inside ozz, zero allocations after construction):
//   1. SamplingJob    x N locomotion clips, phase-synchronised (normalised time)
//   2. BlendingJob    1D blend space by ground speed (idle / walk / run ...)
//   3. LocalToModelJob
//   4. Foot IK on volumetric terrain: ITerrainQuery raycasts under each ankle,
//      pelvis lowered by the largest drop, IKTwoBoneJob per leg, corrections
//      folded back into the local pose, LocalToModel re-run from the hips.
//   5. Skinning palette = model * inverseBind (row-major float4x4 for upload).
//
// Thread safety: one instance per character; update() may run on any worker
// (ozz jobs are stateless), so characters animate in parallel via parallelFor.
// The class keeps ozz types out of the header (pimpl).
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vxe {

class ITerrainQuery;

struct LegRigDesc {
    std::string hip, knee, ankle;
    float ankleHeight = 0.08f;     // ankle joint height above the sole
};

struct LocomotionClipDesc {
    std::string name;
    float speed;                   // m/s the clip was authored for (blend-space coordinate)
};

using Mat4 = std::array<float, 16>;   // column-major (matches ozz::math::Float4x4 / HLSL column_major)

class CharacterAnimator {
public:
    struct Rig;   // opaque: skeleton + clips (ozz)

    explicit CharacterAnimator(std::shared_ptr<const Rig> rig);
    ~CharacterAnimator();
    CharacterAnimator(CharacterAnimator&&) noexcept;
    CharacterAnimator& operator=(CharacterAnimator&&) noexcept;

    // Build a rig from runtime ozz archives produced by the asset pipeline
    // (.ozz skeleton + one .ozz per locomotion clip, sorted by speed).
    static std::shared_ptr<const Rig> loadRig(const std::string& skeletonPath,
                                              const std::vector<std::string>& clipPaths,
                                              const std::vector<LocomotionClipDesc>& clips,
                                              const LegRigDesc& left, const LegRigDesc& right);
    // Procedural biped (root/pelvis/2x thigh-shin-foot) with idle/walk/run clips.
    // Used by tests and as a placeholder before art lands.
    static std::shared_ptr<const Rig> makeProceduralBiped();

    void update(float dt, float groundSpeed, const Vec3d& rootWorld, float yawRadians, const ITerrainQuery* terrain);

    int jointCount() const;
    std::span<const Mat4> modelMatrices() const { return m_model; }
    void skinningPalette(std::span<Mat4> out) const;
    float pelvisOffset() const { return m_pelvisOffset; }
    bool leftFootReached() const { return m_reached[0]; }
    bool rightFootReached() const { return m_reached[1]; }
    float phase() const { return m_phase; }

private:
    struct State;
    std::shared_ptr<const Rig> m_rig;
    std::unique_ptr<State> m_state;
    std::vector<Mat4> m_model;
    float m_phase = 0.f;
    float m_pelvisOffset = 0.f;
    bool m_reached[2] = {false, false};
};

} // namespace vxe
