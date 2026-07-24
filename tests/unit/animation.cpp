// Skeletal animation math: pure CPU, no Context, no Vulkan. Skeleton::ComputeSkinningMatrices
// and SampleAnimationPose are glm-only functions of a bone table + keyframes, so these run
// with no ICD (the bvh.cpp / punctual_shadows.cpp pattern). The properties: a bind-pose
// skeleton skins to identity, a bone without an animation channel holds its bind pose, and a
// keyed bone's pose changes over time.

#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/AnimationSystem.h>

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
