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
