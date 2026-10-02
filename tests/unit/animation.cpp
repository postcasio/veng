// Skeletal animation math: pure CPU, no Context, no Vulkan. Skeleton::ComputeSkinningMatrices
// and SampleAnimationPose are glm-only functions of a bone table + keyframes, so these run
// with no ICD (the bvh.cpp / punctual_shadows.cpp pattern). The properties: a bind-pose
// skeleton skins to identity, a bone without an animation channel holds its bind pose, and a
// keyed bone's pose changes over time. The procedural joint cases run the pure pose helpers, then
// AnimationSystem itself over a skinned mesh whose skeleton loads from a memory mount through a
// manager whose Context is never initialized.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Input.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/AnimationBlend.h>
#include <Veng/Scene/AnimationSystem.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

#include "support/CookedSkeleton.h"

using namespace Veng;

namespace
{
    // A two-bone skeleton: a root at the origin and a child translated +1 in Y. Each bone's
    // inverse-bind is the inverse of its global bind transform, so the bind pose skins to
    // identity (the canonical skinning invariant).
    Skeleton MakeSkeleton()
    {
        Skeleton skeleton;
        skeleton.GlobalInverse = mat4(1.0f);

        Bone root;
        root.Parent = -1;
        root.Name = "Root";
        root.LocalPosition = vec3(0.0f);
        root.LocalRotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        root.LocalScale = vec3(1.0f);
        root.InverseBind = mat4(1.0f);

        Bone child;
        child.Parent = 0;
        child.Name = "Child";
        child.LocalPosition = vec3(0.0f, 1.0f, 0.0f);
        child.LocalRotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        child.LocalScale = vec3(1.0f);
        // Global bind = translate(0,1,0); inverse-bind is its inverse.
        child.InverseBind = glm::inverse(glm::translate(mat4(1.0f), vec3(0.0f, 1.0f, 0.0f)));

        skeleton.Bones = {root, child};
        return skeleton;
    }

    bool IsApproxIdentity(const mat4& m)
    {
        const mat4 id(1.0f);
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (m[c][r] != doctest::Approx(id[c][r]).epsilon(1e-4))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsFinite(const mat4& m)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (!std::isfinite(m[c][r]))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("bind pose skins to identity")
{
    const Skeleton skeleton = MakeSkeleton();
    vector<mat4> palette;
    skeleton.ComputeBindPoseMatrices(palette);

    REQUIRE(palette.size() == 2);
    CHECK(IsApproxIdentity(palette[0]));
    CHECK(IsApproxIdentity(palette[1]));
}

TEST_CASE("sampling holds bind pose at the bind values")
{
    const Skeleton skeleton = MakeSkeleton();

    // An animation that keys the child's rotation: identity at t=0, 90° about Z at t=1.
    Animation animation;
    animation.Duration = 1.0f;
    AnimationChannel channel;
    channel.BoneIndex = 1;
    channel.Rotation = {
        QuatKey{.Time = 0.0f, .Value = quat(1.0f, 0.0f, 0.0f, 0.0f)},
        QuatKey{.Time = 1.0f, .Value = glm::angleAxis(glm::radians(90.0f), vec3(0, 0, 1))},
    };
    animation.Channels = {channel};

    vector<mat4> localPose;
    SampleAnimationPose(skeleton, animation, 0.0f, false, localPose);
    REQUIRE(localPose.size() == 2);

    // The root has no channel: it stays at its (identity) bind local.
    CHECK(IsApproxIdentity(localPose[0]));
    // At t=0 the child's keyed rotation is identity, so its local equals the bind local
    // (translate(0,1,0)).
    const mat4 bindChild = glm::translate(mat4(1.0f), vec3(0.0f, 1.0f, 0.0f));
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            CHECK(localPose[1][c][r] == doctest::Approx(bindChild[c][r]).epsilon(1e-4));
        }
    }

    vector<mat4> palette;
    skeleton.ComputeSkinningMatrices(localPose, palette);
    CHECK(IsApproxIdentity(palette[0]));
    CHECK(IsApproxIdentity(palette[1]));
}

TEST_CASE("a keyed bone's pose changes over time")
{
    const Skeleton skeleton = MakeSkeleton();

    Animation animation;
    animation.Duration = 1.0f;
    AnimationChannel channel;
    channel.BoneIndex = 1;
    channel.Rotation = {
        QuatKey{.Time = 0.0f, .Value = quat(1.0f, 0.0f, 0.0f, 0.0f)},
        QuatKey{.Time = 1.0f, .Value = glm::angleAxis(glm::radians(90.0f), vec3(0, 0, 1))},
    };
    animation.Channels = {channel};

    vector<mat4> poseStart;
    vector<mat4> poseMid;
    SampleAnimationPose(skeleton, animation, 0.0f, false, poseStart);
    SampleAnimationPose(skeleton, animation, 0.5f, false, poseMid);

    vector<mat4> paletteStart;
    vector<mat4> paletteMid;
    skeleton.ComputeSkinningMatrices(poseStart, paletteStart);
    skeleton.ComputeSkinningMatrices(poseMid, paletteMid);

    // The root never moves; the keyed child does.
    CHECK(IsApproxIdentity(paletteMid[0]));
    CHECK(IsFinite(paletteMid[1]));
    CHECK_FALSE(IsApproxIdentity(paletteMid[1]));
}

TEST_CASE("looping wraps the sample time")
{
    const Skeleton skeleton = MakeSkeleton();

    Animation animation;
    animation.Duration = 1.0f;
    AnimationChannel channel;
    channel.BoneIndex = 1;
    channel.Rotation = {
        QuatKey{.Time = 0.0f, .Value = quat(1.0f, 0.0f, 0.0f, 0.0f)},
        QuatKey{.Time = 1.0f, .Value = glm::angleAxis(glm::radians(90.0f), vec3(0, 0, 1))},
    };
    animation.Channels = {channel};

    // t = 2.5 with looping wraps to 0.5; the result matches a direct 0.5 sample.
    vector<mat4> wrapped;
    vector<mat4> direct;
    SampleAnimationPose(skeleton, animation, 2.5f, true, wrapped);
    SampleAnimationPose(skeleton, animation, 0.5f, false, direct);

    REQUIRE(wrapped.size() == direct.size());
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            CHECK(wrapped[1][c][r] == doctest::Approx(direct[1][c][r]).epsilon(1e-4));
        }
    }
}

namespace
{
    bool ApproxEqual(const vec3 a, const vec3 b)
    {
        return a.x == doctest::Approx(b.x).epsilon(1e-4) &&
               a.y == doctest::Approx(b.y).epsilon(1e-4) &&
               a.z == doctest::Approx(b.z).epsilon(1e-4);
    }

    // Quaternions q and -q are the same rotation, so compare by |dot| ~ 1.
    bool SameRotation(const quat a, const quat b)
    {
        return std::abs(glm::dot(a, b)) == doctest::Approx(1.0f).epsilon(1e-4);
    }

    // A one-bone (root) rotation clip: identity at t=0, `degrees` about Z at t=Duration, linear.
    Animation MakeRotationClip(const f32 duration, const f32 degrees)
    {
        Animation animation;
        animation.Duration = duration;
        AnimationChannel channel;
        channel.BoneIndex = 1;
        channel.Rotation = {
            QuatKey{.Time = 0.0f, .Value = quat(1.0f, 0.0f, 0.0f, 0.0f)},
            QuatKey{.Time = duration,
                    .Value = glm::angleAxis(glm::radians(degrees), vec3(0, 0, 1))},
        };
        animation.Channels = {channel};
        return animation;
    }
}

TEST_CASE("blend bracket resolves thresholds")
{
    const vector<f32> thresholds = {0.0f, 2.0f, 4.0f, 6.0f};

    SUBCASE("between two thresholds is the normalized distance")
    {
        const BlendBracket bracket = FindBlendBracket(thresholds, 3.0f);
        CHECK(bracket.Lo == 1);
        CHECK(bracket.Hi == 2);
        CHECK(bracket.Weight == doctest::Approx(0.5f));
    }

    SUBCASE("exactly on a threshold is that clip at full weight")
    {
        const BlendBracket bracket = FindBlendBracket(thresholds, 4.0f);
        // Weight 0 on Hi means the pose is exactly sample Lo — the threshold's own clip.
        CHECK(bracket.Lo == 2);
        CHECK(bracket.Weight == doctest::Approx(0.0f));
    }

    SUBCASE("below the first threshold is the first clip")
    {
        const BlendBracket bracket = FindBlendBracket(thresholds, -5.0f);
        CHECK(bracket.Lo == 0);
        CHECK(bracket.Hi == 0);
        CHECK(bracket.Weight == doctest::Approx(0.0f));
    }

    SUBCASE("above the last threshold is the last clip")
    {
        const BlendBracket bracket = FindBlendBracket(thresholds, 100.0f);
        CHECK(bracket.Lo == 3);
        CHECK(bracket.Hi == 3);
        CHECK(bracket.Weight == doctest::Approx(0.0f));
    }
}

TEST_CASE("a parameter halfway blends each joint halfway")
{
    // Two single-bone poses: rotation 0 vs 90 about Z, translation (0,1,0) vs (2,1,0).
    const vector<JointPose> a = {
        JointPose{.Translation = vec3(0.0f),
                  .Rotation = quat(1.0f, 0.0f, 0.0f, 0.0f),
                  .Scale = vec3(1.0f)},
    };
    const vector<JointPose> b = {
        JointPose{.Translation = vec3(2.0f, 0.0f, 0.0f),
                  .Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0, 0, 1)),
                  .Scale = vec3(3.0f)},
    };

    vector<JointPose> mid;
    BlendLocalPoses(a, b, 0.5f, mid);
    REQUIRE(mid.size() == 1);

    // Translation and scale lerp; rotation slerps to the 45 halfway.
    CHECK(ApproxEqual(mid[0].Translation, vec3(1.0f, 0.0f, 0.0f)));
    CHECK(ApproxEqual(mid[0].Scale, vec3(2.0f)));
    CHECK(SameRotation(mid[0].Rotation, glm::angleAxis(glm::radians(45.0f), vec3(0, 0, 1))));

    // At weight 0 and 1 the blend is exactly the endpoint pose (the at-threshold property).
    vector<JointPose> endLo;
    vector<JointPose> endHi;
    BlendLocalPoses(a, b, 0.0f, endLo);
    BlendLocalPoses(a, b, 1.0f, endHi);
    CHECK(ApproxEqual(endLo[0].Translation, a[0].Translation));
    CHECK(SameRotation(endLo[0].Rotation, a[0].Rotation));
    CHECK(ApproxEqual(endHi[0].Translation, b[0].Translation));
    CHECK(SameRotation(endHi[0].Rotation, b[0].Rotation));
}

TEST_CASE("the TRS sample composes to the same matrix as the direct sample")
{
    const Skeleton skeleton = MakeSkeleton();
    const Animation clip = MakeRotationClip(1.0f, 90.0f);

    for (const f32 t : {0.0f, 0.25f, 0.5f, 0.9f})
    {
        vector<mat4> direct;
        SampleAnimationPose(skeleton, clip, t, false, direct);

        vector<JointPose> trs;
        SampleAnimationLocalPose(skeleton, clip, t, false, trs);
        vector<mat4> composed;
        ComposeLocalPose(trs, composed);

        REQUIRE(direct.size() == composed.size());
        for (usize b = 0; b < direct.size(); ++b)
        {
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    CHECK(composed[b][c][r] == doctest::Approx(direct[b][c][r]).epsilon(1e-4));
                }
            }
        }
    }
}

TEST_CASE("phase-synchronized sampling keeps different-duration clips aligned")
{
    const Skeleton skeleton = MakeSkeleton();
    // Two clips with the same normalized keyframe layout but different durations — the classic
    // walk-vs-run pair. Sampling each at phase * its own duration must keep them at the identical
    // normalized pose at every phase; on independent clocks they would slide apart.
    const Animation slow = MakeRotationClip(1.1f, 90.0f);
    const Animation fast = MakeRotationClip(0.7f, 90.0f);

    constexpr int Samples = 1000;
    for (int i = 0; i < Samples; ++i)
    {
        const f32 phase = static_cast<f32>(i) / static_cast<f32>(Samples);

        vector<JointPose> poseSlow;
        vector<JointPose> poseFast;
        SampleAnimationLocalPose(skeleton, slow, phase * slow.Duration, true, poseSlow);
        SampleAnimationLocalPose(skeleton, fast, phase * fast.Duration, true, poseFast);

        REQUIRE(poseSlow.size() == 2);
        // The keyed bone stays at the same normalized rotation for both durations, every sample.
        CHECK(SameRotation(poseSlow[1].Rotation, poseFast[1].Rotation));
    }
}

TEST_CASE("crossfade weight ramps monotonically to one with no overshoot")
{
    constexpr f32 FadeIn = 0.5f;
    constexpr f32 Delta = 1.0f / 60.0f;

    f32 weight = 0.0f;
    f32 previous = -1.0f;
    bool reachedOne = false;
    for (int i = 0; i < 120; ++i)
    {
        weight = AdvanceCrossfade(weight, FadeIn, Delta);
        // Monotonic non-decreasing, and never past 1 (no discontinuity at either end).
        CHECK(weight >= previous);
        CHECK(weight <= 1.0f);
        previous = weight;
        if (weight >= 1.0f)
        {
            reachedOne = true;
        }
    }
    // FadeIn seconds at 60 Hz is 30 ticks; 120 ticks in, it is pinned at exactly 1.
    CHECK(reachedOne);
    CHECK(weight == doctest::Approx(1.0f));

    // A zero fade completes in a single tick.
    CHECK(AdvanceCrossfade(0.0f, 0.0f, Delta) == doctest::Approx(1.0f));
}

namespace
{
    constexpr usize RigRoot = 0;
    constexpr usize RigArm = 1;
    constexpr usize RigHand = 2;
    constexpr usize RigSide = 3;

    quat QuarterTurn(const vec3& axis)
    {
        return glm::angleAxis(glm::radians(90.0f), axis);
    }

    // Where a joint sits in mesh space under a pose: the translation of its model transform.
    vec3 JointPosition(const Skeleton& skeleton, const vector<mat4>& localPose, const usize joint)
    {
        return vec3(skeleton.JointModelTransform(localPose, joint)[3]);
    }

    // Where a joint's rest position lands under a skinning palette, as a vertex bound to it would.
    vec3 SkinnedRestPosition(const vector<mat4>& skinning, const usize joint)
    {
        return vec3(skinning[joint] * vec4(TestSupport::ArmRigRestPosition(joint), 1.0f));
    }

    bool ApproxEqual(const mat4& a, const mat4& b)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (a[c][r] != doctest::Approx(b[c][r]).epsilon(1e-4))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // A clip turning the rig's Arm a quarter about +Z over one second.
    Animation ArmClip()
    {
        Animation animation;
        animation.Duration = 1.0f;
        AnimationChannel channel;
        channel.BoneIndex = static_cast<u32>(RigArm);
        channel.Rotation = {
            QuatKey{.Time = 0.0f, .Value = quat(1.0f, 0.0f, 0.0f, 0.0f)},
            QuatKey{.Time = 1.0f, .Value = QuarterTurn(vec3(0.0f, 0.0f, 1.0f))},
        };
        animation.Channels = {channel};
        return animation;
    }
}

TEST_CASE("a joint rotation turns exactly that joint's subtree")
{
    const Skeleton rig = TestSupport::MakeArmRig();
    vector<mat4> bind;
    rig.ComputeBindLocalPose(bind);

    const JointRotation turn[] = {{.Joint = RigArm, .Rotation = QuarterTurn(vec3(0, 0, 1))}};
    vector<mat4> posed;
    rig.ComputeLocalPose(turn, posed);
    REQUIRE(posed.size() == rig.GetBoneCount());

    // Above and beside the turned joint nothing moves: the root and the sibling keep their frames.
    CHECK(ApproxEqual(rig.JointModelTransform(posed, RigRoot),
                      rig.JointModelTransform(bind, RigRoot)));
    CHECK(ApproxEqual(rig.JointModelTransform(posed, RigSide),
                      rig.JointModelTransform(bind, RigSide)));

    // The joint turns about its own origin, so it stays put; its child swings with it.
    CHECK(ApproxEqual(JointPosition(rig, posed, RigArm), TestSupport::ArmRigRestPosition(RigArm)));
    CHECK(ApproxEqual(JointPosition(rig, posed, RigHand),
                      TestSupport::ArmRigRestPosition(RigArm) + vec3(0.0f, 1.0f, 0.0f)));

    // In the palette only the subtree departs from identity.
    vector<mat4> skinning;
    rig.ComputeSkinningMatrices(posed, skinning);
    CHECK(IsApproxIdentity(skinning[RigRoot]));
    CHECK(IsApproxIdentity(skinning[RigSide]));
    CHECK_FALSE(IsApproxIdentity(skinning[RigArm]));
    CHECK_FALSE(IsApproxIdentity(skinning[RigHand]));
}

TEST_CASE("a joint's model transform is the frame the skinning palette uses")
{
    const Skeleton rig = TestSupport::MakeArmRig();

    // At rest a joint sits at its authored mesh-space position, global inverse included.
    vector<mat4> bind;
    rig.ComputeBindLocalPose(bind);
    CHECK(ApproxEqual(JointPosition(rig, bind, RigHand), TestSupport::ArmRigRestPosition(RigHand)));

    const JointRotation turns[] = {
        {.Joint = RigRoot, .Rotation = QuarterTurn(vec3(0, 1, 0))},
        {.Joint = RigArm, .Rotation = QuarterTurn(vec3(1, 0, 0))},
    };
    vector<mat4> posed;
    rig.ComputeLocalPose(turns, posed);
    vector<mat4> skinning;
    rig.ComputeSkinningMatrices(posed, skinning);
    for (usize joint = 0; joint < rig.GetBoneCount(); ++joint)
    {
        CHECK(ApproxEqual(rig.JointModelTransform(posed, joint) * rig.Bones[joint].InverseBind,
                          skinning[joint]));
    }
    // So the leaf joint is where a vertex skinned to it at its rest position is drawn.
    CHECK(ApproxEqual(JointPosition(rig, posed, RigHand), SkinnedRestPosition(skinning, RigHand)));

    const mat4 world = glm::translate(mat4(1.0f), vec3(10.0f, 0.0f, -3.0f)) *
                       glm::mat4_cast(QuarterTurn(vec3(0, 1, 0)));
    CHECK(ApproxEqual(rig.JointWorldTransform(world, posed, RigHand),
                      world * rig.JointModelTransform(posed, RigHand)));
}

TEST_CASE("a joint rotation composes onto a sampled clip pose")
{
    const Skeleton rig = TestSupport::MakeArmRig();
    const Animation clip = ArmClip();

    vector<JointPose> sampled;
    SampleAnimationLocalPose(rig, clip, 1.0f, false, sampled);
    vector<JointPose> overridden = sampled;
    const quat spin = QuarterTurn(vec3(0, 0, 1));
    const JointRotation turn[] = {{.Joint = RigArm, .Rotation = spin}};
    ApplyJointRotations(turn, overridden);

    CHECK(SameRotation(overridden[RigArm].Rotation, sampled[RigArm].Rotation * spin));
    CHECK(ApproxEqual(overridden[RigArm].Translation, sampled[RigArm].Translation));
    for (const usize joint : {RigRoot, RigHand, RigSide})
    {
        CHECK(SameRotation(overridden[joint].Rotation, sampled[joint].Rotation));
    }

    // The clip's quarter turn and the override's quarter turn add: the hand ends up behind the arm.
    vector<mat4> local;
    ComposeLocalPose(overridden, local);
    CHECK(ApproxEqual(JointPosition(rig, local, RigHand),
                      TestSupport::ArmRigRestPosition(RigArm) + vec3(-1.0f, 0.0f, 0.0f)));
}

namespace
{
    constexpr AssetId RigSkeletonId{0x3E5C9BDAC94837A0ULL};

    // A scene drawing a skinned mesh over the arm rig, with the system and its context at hand.
    struct PosingScene
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Assets;
        MountHandle Mount;
        Unique<Scene> World;
        AssetHandle<Mesh> RigMesh;
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char Unused[64]{};
        AnimationSystem System;

        PosingScene()
        {
            RegisterBuiltinTypes(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            ArchiveWriter writer;
            writer.Add(RigSkeletonId, AssetTypes::Skeleton,
                       TestSupport::CookSkeleton(TestSupport::MakeArmRig()));
            Mount = Assets->MountMemory(writer.Build(), "arm_rig");
            const AssetResult<AssetHandle<Skeleton>> skeleton =
                Assets->LoadSync<Skeleton>(RigSkeletonId);
            REQUIRE(skeleton.has_value());
            RigMesh =
                Assets->Adopt<Mesh>(Mesh::Create(MeshInfo{.Name = "rig", .Skeleton = *skeleton}));
            World = Scene::Create(Types);
        }

        Entity Spawn()
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{});
            World->Add<MeshRenderer>(entity, MeshRenderer{.Mesh = RigMesh});
            return entity;
        }

        void Tick()
        {
            const SystemContext context{
                .Assets = *Assets,
                .Input = HeadlessInput,
                .Tasks = Tasks,
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(Unused),
                .Localization = *reinterpret_cast<Localization::Localization*>(Unused),
            };
            System.OnUpdate(*World, 1.0f / 60.0f, context);
        }

        [[nodiscard]] const vector<mat4>& Skinning(const Entity entity) const
        {
            return World->Get<SkinnedPose>(entity).Skinning;
        }
    };
}

TEST_CASE("AnimationSystem poses a clip-less entity's joint overrides from the bind pose")
{
    PosingScene posing;
    const Entity entity = posing.Spawn();
    posing.World->Add<JointOverrides>(
        entity,
        JointOverrides{.Entries = {
                           {.Joint = "Missing", .LocalRotation = QuarterTurn(vec3(1, 0, 0))},
                           {.Joint = "Arm", .LocalRotation = QuarterTurn(vec3(0, 0, 1))},
                       }});

    usize warnings = 0;
    Log::SetSink(
        [&](const Log::Level level, std::string_view)
        {
            if (level == Log::Level::Warn)
            {
                ++warnings;
            }
        });
    posing.Tick();
    REQUIRE(posing.World->Has<SkinnedPose>(entity));
    const vector<mat4> first = posing.Skinning(entity);

    // Only the named, known joint's subtree moves; the unknown name is ignored.
    CHECK(IsApproxIdentity(first[RigRoot]));
    CHECK(IsApproxIdentity(first[RigSide]));
    CHECK(ApproxEqual(SkinnedRestPosition(first, RigHand),
                      TestSupport::ArmRigRestPosition(RigArm) + vec3(0.0f, 1.0f, 0.0f)));

    // Rewriting a rotation re-poses without re-resolving, so the unknown name warns once.
    posing.World->Get<JointOverrides>(entity).Entries[1].LocalRotation = quat(1, 0, 0, 0);
    posing.Tick();
    Log::SetSink(nullptr);
    CHECK(warnings == 1);
    for (const mat4& skin : posing.Skinning(entity))
    {
        CHECK(IsApproxIdentity(skin));
    }
}

TEST_CASE("AnimationSystem composes joint overrides onto an Animator's clip")
{
    PosingScene posing;
    const Entity entity = posing.Spawn();
    posing.World->Add<Animator>(
        entity, Animator{.Clip = posing.Assets->Adopt<Animation>(CreateRef<Animation>(ArmClip())),
                         .Time = 1.0f,
                         .Loop = false,
                         .Playing = false});
    posing.World->Add<JointOverrides>(
        entity,
        JointOverrides{.Entries = {{.Joint = "Arm", .LocalRotation = QuarterTurn(vec3(0, 0, 1))}}});

    posing.Tick();
    REQUIRE(posing.World->Has<SkinnedPose>(entity));
    const vector<mat4>& skinning = posing.Skinning(entity);

    // The clip's quarter turn and the override's add to a half turn; the sibling is untouched.
    CHECK(ApproxEqual(SkinnedRestPosition(skinning, RigHand),
                      TestSupport::ArmRigRestPosition(RigArm) + vec3(-1.0f, 0.0f, 0.0f)));
    CHECK(IsApproxIdentity(skinning[RigSide]));
}

TEST_CASE("direct bone composition equals translate times rotate times scale")
{
    const vector<JointPose> pose = {
        {},
        {.Translation = vec3(1.5f, -2.0f, 0.25f),
         .Rotation = QuarterTurn(vec3(0, 0, 1)),
         .Scale = vec3(2.0f, 0.5f, 3.0f)},
        {.Translation = vec3(-4.0f, 0.0f, 7.0f),
         .Rotation = glm::angleAxis(glm::radians(37.0f), glm::normalize(vec3(1, 2, 3))),
         .Scale = vec3(0.1f, 4.0f, 1.0f)},
        {.Translation = vec3(0.0f, 10.0f, 0.0f),
         .Rotation = glm::angleAxis(glm::radians(-150.0f), vec3(0, 1, 0)),
         .Scale = vec3(-1.0f, 1.0f, 2.5f)},
    };

    // ComposeLocalPose is the same composition, per bone.
    vector<mat4> local;
    ComposeLocalPose(pose, local);
    REQUIRE(local.size() == pose.size());

    f32 worst = 0.0f;
    for (usize i = 0; i < pose.size(); ++i)
    {
        const mat4 expected = glm::translate(mat4(1.0f), pose[i].Translation) *
                              glm::mat4_cast(pose[i].Rotation) *
                              glm::scale(mat4(1.0f), pose[i].Scale);
        const mat4 direct =
            ComposeBoneTransform(pose[i].Translation, pose[i].Rotation, pose[i].Scale);
        CHECK(local[i] == direct);
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                worst = std::max(worst, std::abs(direct[col][row] - expected[col][row]));
            }
        }
    }
    CHECK(worst <= 1e-5f);
}

TEST_CASE("AnimationSystem poses a settled one-shot clip once and re-poses when its input changes")
{
    PosingScene posing;
    const Entity entity = posing.Spawn();
    // Already past the clip's end and still playing: the clamped time is the end on every frame.
    posing.World->Add<Animator>(
        entity, Animator{.Clip = posing.Assets->Adopt<Animation>(CreateRef<Animation>(ArmClip())),
                         .Time = 2.0f,
                         .Loop = false,
                         .Playing = true});

    // The clip's end pose, from the pure sampling path.
    const Skeleton rig = TestSupport::MakeArmRig();
    vector<mat4> endLocal;
    SampleAnimationPose(rig, ArmClip(), 1.0f, false, endLocal);
    vector<mat4> endSkinning;
    rig.ComputeSkinningMatrices(endLocal, endSkinning);

    posing.Tick();
    REQUIRE(posing.World->Has<SkinnedPose>(entity));
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 1);
    CHECK(ApproxEqual(posing.Skinning(entity)[RigHand], endSkinning[RigHand]));

    for (int frame = 0; frame < 60; ++frame)
    {
        posing.Tick();
    }
    // The clock kept running; the pose was not recomputed.
    CHECK(posing.World->Get<Animator>(entity).Time > 2.9f);
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 1);

    // Rewinding into the clip changes the sampled time, so the very next frame re-poses.
    posing.World->Get<Animator>(entity).Time = 0.0f;
    posing.Tick();
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 2);
    CHECK_FALSE(ApproxEqual(posing.Skinning(entity)[RigHand], endSkinning[RigHand]));
}

TEST_CASE("AnimationSystem poses a settled state once and re-poses on the frame it is released")
{
    PosingScene posing;
    const Entity entity = posing.Spawn();
    posing.World->Add<Animator>(entity, Animator{});
    posing.World->Add<AnimationStateSet>(
        entity, AnimationStateSet{.States = {AnimationState{.Name = "Deploy",
                                                            .Clip = posing.Assets->Adopt<Animation>(
                                                                CreateRef<Animation>(ArmClip())),
                                                            .Loop = false,
                                                            .FadeIn = 0.0f}},
                                  .RequestedState = "Deploy"});

    // A one-second one-shot at 60 Hz has finished well within 90 frames.
    for (int frame = 0; frame < 90; ++frame)
    {
        posing.Tick();
    }
    const u64 settled = posing.World->Get<SkinnedPose>(entity).Version;
    for (int frame = 0; frame < 30; ++frame)
    {
        posing.Tick();
    }
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == settled);

    posing.World->Get<AnimationStateSet>(entity).RequestedState.clear();
    posing.Tick();
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == settled + 1);
    for (const mat4& skin : posing.Skinning(entity))
    {
        CHECK(IsApproxIdentity(skin));
    }
}

TEST_CASE("AnimationSystem re-poses joint overrides only when a rotation's value changes")
{
    PosingScene posing;
    const Entity entity = posing.Spawn();
    const quat turn = QuarterTurn(vec3(0, 0, 1));
    posing.World->Add<JointOverrides>(
        entity, JointOverrides{.Entries = {{.Joint = "Arm", .LocalRotation = turn}}});

    for (int frame = 0; frame < 30; ++frame)
    {
        posing.Tick();
    }
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 1);

    // Rewriting the identical value is not a change.
    posing.World->Get<JointOverrides>(entity).Entries[0].LocalRotation = turn;
    posing.Tick();
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 1);

    posing.World->Get<JointOverrides>(entity).Entries[0].LocalRotation = quat(1, 0, 0, 0);
    posing.Tick();
    CHECK(posing.World->Get<SkinnedPose>(entity).Version == 2);
    for (const mat4& skin : posing.Skinning(entity))
    {
        CHECK(IsApproxIdentity(skin));
    }
}
