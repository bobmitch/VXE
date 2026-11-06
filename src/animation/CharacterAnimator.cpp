#include "vxe/animation/CharacterAnimator.h"

#include "vxe/building/TerrainQuery.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/transform.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vxe {

namespace oa = ozz::animation;
namespace om = ozz::math;

struct CharacterAnimator::Rig {
    ozz::unique_ptr<oa::Skeleton> skeleton;
    std::vector<ozz::unique_ptr<oa::Animation>> clips;
    std::vector<float> speeds;           // ascending
    int leg[2][3] = {{-1, -1, -1}, {-1, -1, -1}};
    float ankleHeight[2] = {0.08f, 0.08f};
    om::SimdFloat4 kneeAxis = om::simd_float4::x_axis();
    std::vector<om::Float4x4> inverseBind;
};

struct CharacterAnimator::State {
    std::vector<std::unique_ptr<oa::SamplingJob::Context>> contexts;
    std::vector<ozz::vector<om::SoaTransform>> clipLocals;
    ozz::vector<om::SoaTransform> blended;
    ozz::vector<om::Float4x4> models;
    std::vector<oa::BlendingJob::Layer> layers;
};

namespace {

int findJoint(const oa::Skeleton& s, const std::string& name) {
    const auto names = s.joint_names();
    for (size_t i = 0; i < names.size(); ++i)
        if (name == names[i]) return int(i);
    return -1;
}

void finaliseRig(CharacterAnimator::Rig& rig, const LegRigDesc& l, const LegRigDesc& r);

// ozz sample-framework helper: rotate one joint of a SoA local pose.
void multiplySoaQuaternion(int joint, const om::SimdQuaternion& q, ozz::span<om::SoaTransform> locals) {
    om::SoaTransform& soa = locals[size_t(joint / 4)];
    om::SimdQuaternion aos[4];
    om::Transpose4x4(&soa.rotation.x, &aos->xyzw);
    aos[joint & 3] = aos[joint & 3] * q;
    om::Transpose4x4(&aos->xyzw, &soa.rotation.x);
}

void storeMat(const om::Float4x4& m, Mat4& out) {
    for (int c = 0; c < 4; ++c) om::StorePtrU(m.cols[c], out.data() + c * 4);
}

} // namespace

// ---------------------------------------------------------------------------
// Rig construction
// ---------------------------------------------------------------------------
std::shared_ptr<const CharacterAnimator::Rig> CharacterAnimator::loadRig(const std::string& skeletonPath,
                                                                         const std::vector<std::string>& clipPaths,
                                                                         const std::vector<LocomotionClipDesc>& clips,
                                                                         const LegRigDesc& left, const LegRigDesc& right) {
    auto rig = std::make_shared<Rig>();
    {
        ozz::io::File file(skeletonPath.c_str(), "rb");
        if (!file.opened()) throw std::runtime_error("cannot open skeleton " + skeletonPath);
        ozz::io::IArchive ar(&file);
        rig->skeleton = ozz::make_unique<oa::Skeleton>();
        ar >> *rig->skeleton;
    }
    for (size_t i = 0; i < clipPaths.size(); ++i) {
        ozz::io::File file(clipPaths[i].c_str(), "rb");
        if (!file.opened()) throw std::runtime_error("cannot open animation " + clipPaths[i]);
        ozz::io::IArchive ar(&file);
        auto anim = ozz::make_unique<oa::Animation>();
        ar >> *anim;
        rig->clips.push_back(std::move(anim));
        rig->speeds.push_back(clips.at(i).speed);
    }
    finaliseRig(*rig, left, right);
    return rig;
}

std::shared_ptr<const CharacterAnimator::Rig> CharacterAnimator::makeProceduralBiped() {
    auto rig = std::make_shared<Rig>();

    // --- Skeleton: root -> pelvis -> {thigh_l -> shin_l -> foot_l, thigh_r -> ...}
    oa::offline::RawSkeleton raw;
    raw.roots.resize(1);
    auto& root = raw.roots[0];
    root.name = "root";
    root.transform = om::Transform::identity();
    root.children.resize(1);
    auto& pelvis = root.children[0];
    pelvis.name = "pelvis";
    pelvis.transform = om::Transform::identity();
    pelvis.transform.translation = om::Float3(0.f, 0.95f, 0.f);
    pelvis.children.resize(2);
    for (int s = 0; s < 2; ++s) {
        const char* side = s == 0 ? "_l" : "_r";
        auto& thigh = pelvis.children[size_t(s)];
        thigh.name = std::string("thigh") + side;
        thigh.transform = om::Transform::identity();
        thigh.transform.translation = om::Float3(s == 0 ? 0.1f : -0.1f, 0.f, 0.f);
        thigh.children.resize(1);
        auto& shin = thigh.children[0];
        shin.name = std::string("shin") + side;
        shin.transform = om::Transform::identity();
        shin.transform.translation = om::Float3(0.f, -0.45f, 0.f);
        shin.children.resize(1);
        auto& foot = shin.children[0];
        foot.name = std::string("foot") + side;
        foot.transform = om::Transform::identity();
        foot.transform.translation = om::Float3(0.f, -0.42f, 0.f);
    }
    rig->skeleton = oa::offline::SkeletonBuilder()(raw);
    if (!rig->skeleton) throw std::runtime_error("procedural skeleton build failed");

    // --- Clips: idle (0 m/s), walk (1.6 m/s), run (4.5 m/s): sinusoidal thigh/shin swing.
    struct ClipSpec { float speed, duration, thighAmp, kneeAmp; };
    const ClipSpec specs[] = {{0.f, 2.0f, 0.03f, 0.02f}, {1.6f, 1.0f, 0.45f, 0.6f}, {4.5f, 0.7f, 0.8f, 1.1f}};
    const int numJoints = rig->skeleton->num_joints();
    for (const ClipSpec& cs : specs) {
        oa::offline::RawAnimation anim;
        anim.duration = cs.duration;
        anim.tracks.resize(size_t(numJoints));
        const auto restPoses = rig->skeleton->joint_rest_poses();
        (void)restPoses;
        for (int j = 0; j < numJoints; ++j) {
            const std::string name = rig->skeleton->joint_names()[size_t(j)];
            auto& track = anim.tracks[size_t(j)];
            // Static translation key from the raw description (rest pose).
            om::Float3 t = om::Float3::zero();
            if (name == "pelvis") t = om::Float3(0.f, 0.95f, 0.f);
            else if (name.rfind("thigh", 0) == 0) t = om::Float3(name.back() == 'l' ? 0.1f : -0.1f, 0.f, 0.f);
            else if (name.rfind("shin", 0) == 0) t = om::Float3(0.f, -0.45f, 0.f);
            else if (name.rfind("foot", 0) == 0) t = om::Float3(0.f, -0.42f, 0.f);
            track.translations.push_back({0.f, t});
            track.scales.push_back({0.f, om::Float3::one()});
            const float phaseOffset = name.back() == 'r' ? 3.14159265f : 0.f;
            constexpr int kKeys = 9;
            for (int k = 0; k < kKeys; ++k) {
                const float time = cs.duration * float(k) / float(kKeys - 1);
                const float ph = 6.2831853f * float(k) / float(kKeys - 1) + phaseOffset;
                float angle = 0.f;
                if (name.rfind("thigh", 0) == 0) angle = cs.thighAmp * std::sin(ph);
                else if (name.rfind("shin", 0) == 0) angle = cs.kneeAmp * (0.5f + 0.5f * std::sin(ph + 1.2f));
                track.rotations.push_back({time, om::Quaternion::FromAxisAngle(om::Float3::x_axis(), angle)});
            }
        }
        auto built = oa::offline::AnimationBuilder()(anim);
        if (!built) throw std::runtime_error("procedural animation build failed");
        rig->clips.push_back(std::move(built));
        rig->speeds.push_back(cs.speed);
    }
    finaliseRig(*rig, {"thigh_l", "shin_l", "foot_l", 0.06f}, {"thigh_r", "shin_r", "foot_r", 0.06f});
    return rig;
}

namespace {
void finaliseRig(CharacterAnimator::Rig& rig, const LegRigDesc& l, const LegRigDesc& r) {
    const LegRigDesc* legs[2] = {&l, &r};
    for (int s = 0; s < 2; ++s) {
        rig.leg[s][0] = findJoint(*rig.skeleton, legs[s]->hip);
        rig.leg[s][1] = findJoint(*rig.skeleton, legs[s]->knee);
        rig.leg[s][2] = findJoint(*rig.skeleton, legs[s]->ankle);
        rig.ankleHeight[s] = legs[s]->ankleHeight;
    }
    // Inverse bind matrices from the rest pose.
    ozz::vector<om::Float4x4> rest(size_t(rig.skeleton->num_joints()));
    oa::LocalToModelJob ltm;
    ltm.skeleton = rig.skeleton.get();
    ltm.input = rig.skeleton->joint_rest_poses();
    ltm.output = ozz::make_span(rest);
    if (!ltm.Run()) throw std::runtime_error("rest pose LocalToModel failed");
    rig.inverseBind.resize(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) rig.inverseBind[i] = om::Invert(rest[i]);
}
} // namespace

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
CharacterAnimator::CharacterAnimator(std::shared_ptr<const Rig> rig) : m_rig(std::move(rig)), m_state(std::make_unique<State>()) {
    const oa::Skeleton& sk = *m_rig->skeleton;
    const size_t soa = size_t(sk.num_soa_joints());
    for (size_t i = 0; i < m_rig->clips.size(); ++i) {
        m_state->contexts.push_back(std::make_unique<oa::SamplingJob::Context>(m_rig->clips[i]->num_tracks()));
        m_state->clipLocals.emplace_back(soa);
    }
    m_state->blended.resize(soa);
    m_state->models.resize(size_t(sk.num_joints()));
    m_state->layers.resize(m_rig->clips.size());
    m_model.resize(size_t(sk.num_joints()));
}
CharacterAnimator::~CharacterAnimator() = default;
CharacterAnimator::CharacterAnimator(CharacterAnimator&&) noexcept = default;
CharacterAnimator& CharacterAnimator::operator=(CharacterAnimator&&) noexcept = default;

int CharacterAnimator::jointCount() const { return m_rig->skeleton->num_joints(); }

void CharacterAnimator::update(float dt, float groundSpeed, const Vec3d& rootWorld, float yaw, const ITerrainQuery* terrain) {
    const Rig& rig = *m_rig;
    State& st = *m_state;
    const size_t n = rig.clips.size();

    // ---- 1D blend-space weights --------------------------------------------
    std::vector<float> w(n, 0.f);
    const float sp = std::clamp(groundSpeed, rig.speeds.front(), rig.speeds.back());
    for (size_t i = 0; i + 1 < n; ++i) {
        if (sp >= rig.speeds[i] && sp <= rig.speeds[i + 1]) {
            const float t = (sp - rig.speeds[i]) / std::max(1e-4f, rig.speeds[i + 1] - rig.speeds[i]);
            w[i] = 1.f - t;
            w[i + 1] = t;
            break;
        }
    }
    if (n == 1) w[0] = 1.f;
    float duration = 0.f;
    for (size_t i = 0; i < n; ++i) duration += w[i] * rig.clips[i]->duration();
    m_phase = std::fmod(m_phase + dt / std::max(duration, 1e-3f), 1.f);

    // ---- Sample + blend ------------------------------------------------------
    for (size_t i = 0; i < n; ++i) {
        st.layers[i].weight = w[i];
        st.layers[i].transform = ozz::make_span(st.clipLocals[i]);
        if (w[i] <= 0.f) continue;
        oa::SamplingJob job;
        job.animation = rig.clips[i].get();
        job.context = st.contexts[i].get();
        job.ratio = m_phase;
        job.output = ozz::make_span(st.clipLocals[i]);
        job.Run();
    }
    oa::BlendingJob blend;
    blend.threshold = 0.1f;
    blend.layers = ozz::span<const oa::BlendingJob::Layer>(st.layers.data(), st.layers.size());
    blend.rest_pose = rig.skeleton->joint_rest_poses();
    blend.output = ozz::make_span(st.blended);
    blend.Run();

    auto runLtm = [&](const om::Float4x4* root) {
        oa::LocalToModelJob ltm;
        ltm.skeleton = rig.skeleton.get();
        ltm.root = root;
        ltm.input = ozz::make_span(st.blended);
        ltm.output = ozz::make_span(st.models);
        ltm.Run();
    };
    runLtm(nullptr);

    // ---- Foot IK against volumetric terrain -----------------------------------
    m_reached[0] = m_reached[1] = false;
    if (terrain && rig.leg[0][2] >= 0 && rig.leg[1][2] >= 0) {
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        auto toWorld = [&](om::SimdFloat4 m) {
            const float x = om::GetX(m), y = om::GetY(m), z = om::GetZ(m);
            return rootWorld + Vec3d(double(cy * x + sy * z), double(y), double(-sy * x + cy * z));
        };
        // Ground-relative IK (keeps the authored swing): the animation assumes a
        // flat floor at rootWorld.y; each ankle is shifted by how far the real
        // terrain under it deviates from that floor.
        float delta[2] = {0.f, 0.f};
        Vec3d target[2];
        for (int s = 0; s < 2; ++s) {
            const Vec3d ankle = toWorld(st.models[size_t(rig.leg[s][2])].cols[3]);
            target[s] = ankle;
            const Vec3d probe{ankle.x, rootWorld.y + 0.6, ankle.z};
            const auto hit = terrain->raycastDown(probe, 1.6f);
            if (!hit) continue;
            const double groundY = probe.y - double(*hit);
            delta[s] = float(groundY - rootWorld.y);
            target[s].y = std::max(ankle.y + double(delta[s]), groundY + double(rig.ankleHeight[s]));
        }
        // Lower the pelvis so the lower foot can reach (never raise: legs would hyper-extend).
        const float desired = std::min(0.f, std::min(delta[0], delta[1]));
        m_pelvisOffset = lerp(m_pelvisOffset, desired, saturate(dt * 12.f));
        const om::Float4x4 rootOffset = om::Float4x4::Translation(om::simd_float4::Load(0.f, m_pelvisOffset, 0.f, 0.f));
        runLtm(&rootOffset);

        for (int s = 0; s < 2; ++s) {
            const Vec3d d = target[s] - rootWorld;                                 // world -> model (undo yaw)
            const float mx = float(cy * d.x - sy * d.z), mz = float(sy * d.x + cy * d.z);
            om::SimdQuaternion startCorr, midCorr;
            bool reached = false;
            oa::IKTwoBoneJob ik;
            ik.target = om::simd_float4::Load(mx, float(d.y), mz, 1.f);
            ik.pole_vector = om::simd_float4::Load(0.f, 0.f, 1.f, 0.f);
            ik.mid_axis = rig.kneeAxis;
            ik.soften = 0.97f;
            ik.weight = 1.f;
            ik.start_joint = &st.models[size_t(rig.leg[s][0])];
            ik.mid_joint = &st.models[size_t(rig.leg[s][1])];
            ik.end_joint = &st.models[size_t(rig.leg[s][2])];
            ik.start_joint_correction = &startCorr;
            ik.mid_joint_correction = &midCorr;
            ik.reached = &reached;
            if (ik.Run()) {
                multiplySoaQuaternion(rig.leg[s][0], startCorr, ozz::make_span(st.blended));
                multiplySoaQuaternion(rig.leg[s][1], midCorr, ozz::make_span(st.blended));
                runLtm(&rootOffset);   // propagate before solving the other leg
            }
            m_reached[s] = reached;
        }
    }

    for (size_t i = 0; i < st.models.size(); ++i) storeMat(st.models[i], m_model[i]);
}

void CharacterAnimator::skinningPalette(std::span<Mat4> out) const {
    const size_t n = std::min(out.size(), m_state->models.size());
    for (size_t i = 0; i < n; ++i) storeMat(m_state->models[i] * m_rig->inverseBind[i], out[i]);
}

} // namespace vxe
