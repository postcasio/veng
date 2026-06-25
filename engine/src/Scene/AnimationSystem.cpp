#include <Veng/Scene/AnimationSystem.h>

#include <cmath>

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    namespace
    {
        // Interpolates a vec3 track at time t, falling back to `bind` when the track is empty.
        vec3 SampleVec3(const vector<Vec3Key>& keys, f32 t, vec3 bind)
        {
            if (keys.empty())
            {
                return bind;
            }
            if (t <= keys.front().Time)
            {
                return keys.front().Value;
            }
            if (t >= keys.back().Time)
            {
                return keys.back().Value;
            }
            for (usize i = 0; i + 1 < keys.size(); ++i)
            {
                if (t < keys[i + 1].Time)
                {
                    const f32 span = keys[i + 1].Time - keys[i].Time;
                    const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
                    return glm::mix(keys[i].Value, keys[i + 1].Value, alpha);
                }
            }
            return keys.back().Value;
        }

        // Finds the channel targeting a given bone, or nullptr when the bone is unanimated.
        const AnimationChannel* ChannelForBone(const Animation& animation, i32 bone)
        {
            for (const AnimationChannel& channel : animation.Channels)
            {
                if (static_cast<i32>(channel.BoneIndex) == bone)
                {
                    return &channel;
                }
            }
            return nullptr;
        }

        // Composes the bind-pose model rotation/scale of a bone's parent chain (root..bone),
        // mapping a translation in the bone's local space into model space. Ancestors above a
        // root-motion bone are treated as static, so their bind pose is their pose.
        mat3 BindModelRotation(const Skeleton& skeleton, i32 bone)
        {
            mat4 model(1.0f);
            for (i32 b = bone; b >= 0; b = skeleton.Bones[static_cast<usize>(b)].Parent)
            {
                model = skeleton.BindLocalMatrix(static_cast<usize>(b)) * model;
            }
            return mat3(model);
        }

        // The per-tick local-space translation of the root-motion bone between two playback
        // times, wrapping/clamping each like SampleAnimationPose. A forward loop wrap is bridged
        // across the clip seam so the extracted stride stays continuous.
        vec3 ExtractRootDelta(const Skeleton& skeleton, const Animation& clip, i32 bone,
                              f32 prevTime, f32 nowTime, bool loop)
        {
            const f32 duration = clip.Duration;
            const auto resolve = [&](f32 t) -> f32
            {
                if (duration <= 0.0f)
                {
                    return 0.0f;
                }
                if (loop)
                {
                    t = std::fmod(t, duration);
                    return t < 0.0f ? t + duration : t;
                }
                return glm::clamp(t, 0.0f, duration);
            };

            const f32 tn = resolve(nowTime);
            const f32 tp = resolve(prevTime);
            const vec3 pn = SampleBoneLocalPosition(skeleton, clip, bone, tn);
            const vec3 pp = SampleBoneLocalPosition(skeleton, clip, bone, tp);

            if (loop && duration > 0.0f && nowTime > prevTime && tn < tp)
            {
                const vec3 pEnd = SampleBoneLocalPosition(skeleton, clip, bone, duration);
                const vec3 pStart = SampleBoneLocalPosition(skeleton, clip, bone, 0.0f);
                return (pEnd - pp) + (pn - pStart);
            }
            return pn - pp;
        }

        // Interpolates a rotation track at time t (slerp), falling back to `bind` when empty.
        quat SampleQuat(const vector<QuatKey>& keys, f32 t, quat bind)
        {
            if (keys.empty())
            {
                return bind;
            }
            if (t <= keys.front().Time)
            {
                return keys.front().Value;
            }
            if (t >= keys.back().Time)
            {
                return keys.back().Value;
            }
            for (usize i = 0; i + 1 < keys.size(); ++i)
            {
                if (t < keys[i + 1].Time)
                {
                    const f32 span = keys[i + 1].Time - keys[i].Time;
                    const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
                    return glm::slerp(keys[i].Value, keys[i + 1].Value, alpha);
                }
            }
            return keys.back().Value;
        }
    }

    void SampleAnimationPose(const Skeleton& skeleton, const Animation& animation, f32 time,
                             bool loop, vector<mat4>& out)
    {
        const usize count = skeleton.Bones.size();
        out.resize(count);
        for (usize i = 0; i < count; ++i)
        {
            out[i] = skeleton.BindLocalMatrix(i);
        }

        f32 t = time;
        if (animation.Duration > 0.0f)
        {
            t = loop ? std::fmod(time, animation.Duration)
                     : glm::clamp(time, 0.0f, animation.Duration);
            if (t < 0.0f)
            {
                t += animation.Duration;
            }
        }

        for (const AnimationChannel& channel : animation.Channels)
        {
            if (channel.BoneIndex >= count)
            {
                continue;
            }
            const Bone& bone = skeleton.Bones[channel.BoneIndex];
            const vec3 position = SampleVec3(channel.Position, t, bone.LocalPosition);
            const quat rotation = SampleQuat(channel.Rotation, t, bone.LocalRotation);
            const vec3 scale = SampleVec3(channel.Scale, t, bone.LocalScale);

            out[channel.BoneIndex] = glm::translate(mat4(1.0f), position) *
                                     glm::mat4_cast(rotation) * glm::scale(mat4(1.0f), scale);
        }
    }

    i32 FindRootMotionBone(const Skeleton& skeleton, const Animation& animation)
    {
        constexpr f32 VaryEpsilon = 1e-4f;

        i32 best = -1;
        for (const AnimationChannel& channel : animation.Channels)
        {
            if (channel.Position.size() < 2 ||
                static_cast<usize>(channel.BoneIndex) >= skeleton.Bones.size())
            {
                continue;
            }

            vec3 lo = channel.Position.front().Value;
            vec3 hi = lo;
            for (const Vec3Key& key : channel.Position)
            {
                lo = glm::min(lo, key.Value);
                hi = glm::max(hi, key.Value);
            }

            const vec3 range = hi - lo;
            if (range.x <= VaryEpsilon && range.y <= VaryEpsilon && range.z <= VaryEpsilon)
            {
                continue;
            }

            // Bones are topological (parent before child), so the smallest index among the
            // varying-position channels is the highest in the hierarchy — the locomotion root.
            const i32 bone = static_cast<i32>(channel.BoneIndex);
            if (best < 0 || bone < best)
            {
                best = bone;
            }
        }
        return best;
    }

    vec3 SampleBoneLocalPosition(const Skeleton& skeleton, const Animation& animation,
                                 const i32 bone, const f32 time)
    {
        const vec3 bind = bone >= 0 && static_cast<usize>(bone) < skeleton.Bones.size()
                              ? skeleton.Bones[static_cast<usize>(bone)].LocalPosition
                              : vec3(0.0f);
        const AnimationChannel* channel = ChannelForBone(animation, bone);
        if (channel == nullptr)
        {
            return bind;
        }
        return SampleVec3(channel->Position, time, bind);
    }

    void AnimationSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& /*context*/)
    {
        const Scene& readScene = scene;

        // Add a SkinnedPose to any animated, resident, skinned-mesh entity that lacks one.
        // Collected first so the structural add never happens mid-iteration.
        vector<Entity> needPose;
        for (auto [entity, animator] : readScene.View<Animator>())
        {
            if (scene.Has<SkinnedPose>(entity))
            {
                continue;
            }
            const auto* renderer = readScene.TryGet<MeshRenderer>(entity);
            if (renderer != nullptr && renderer->Mesh.IsLoaded() && renderer->Mesh->IsSkinned())
            {
                needPose.push_back(entity);
            }
        }
        for (const Entity entity : needPose)
        {
            scene.Add<SkinnedPose>(entity, SkinnedPose{});
        }

        // Advance each animator and write its skinning palette. MeshRenderer is read through the
        // const scene so this View does not bump the spatial version (no broadphase rebuild).
        // Drive-mode root-motion deltas are collected and published after the loop so the
        // RootMotionDelta add never happens mid-iteration.
        vector<Entity> driveEntities;
        vector<vec3> driveDeltas;
        for (auto [entity, animator] : scene.View<Animator>())
        {
            auto* pose = scene.TryGet<SkinnedPose>(entity);
            if (pose == nullptr)
            {
                continue;
            }

            const auto* renderer = readScene.TryGet<MeshRenderer>(entity);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded() || !renderer->Mesh->IsSkinned())
            {
                continue;
            }
            const AssetHandle<Skeleton>& skeletonHandle = renderer->Mesh->GetSkeleton();
            if (!skeletonHandle.IsLoaded())
            {
                continue;
            }

            const f32 prevTime = animator.Time;
            if (animator.Playing)
            {
                animator.Time += delta * animator.Speed;
            }

            if (!animator.Clip.IsLoaded())
            {
                skeletonHandle->ComputeBindPoseMatrices(pose->Skinning);
                continue;
            }

            const Skeleton& skeleton = *skeletonHandle.Get();
            const Animation& clip = *animator.Clip.Get();

            vector<mat4> localPose;
            SampleAnimationPose(skeleton, clip, animator.Time, animator.Loop, localPose);

            const i32 rootBone = FindRootMotionBone(skeleton, clip);
            if (rootBone >= 0 && static_cast<usize>(rootBone) < localPose.size())
            {
                // Strip the baked translation from the rendered pose: the root bone keeps its
                // animated rotation/scale but holds its bind-pose position. Column 3 of the
                // composed local matrix is exactly that translation.
                const vec3 bindPosition =
                    skeleton.Bones[static_cast<usize>(rootBone)].LocalPosition;
                localPose[static_cast<usize>(rootBone)][3] = vec4(bindPosition, 1.0f);

                if (animator.RootMotion != RootMotionMode::Discard)
                {
                    const vec3 localDelta = ExtractRootDelta(skeleton, clip, rootBone, prevTime,
                                                             animator.Time, animator.Loop);
                    const vec3 modelDelta =
                        BindModelRotation(skeleton,
                                          skeleton.Bones[static_cast<usize>(rootBone)].Parent) *
                        localDelta;

                    if (animator.RootMotion == RootMotionMode::Presentation)
                    {
                        if (auto* transform = scene.TryGet<Transform>(entity))
                        {
                            transform->Position +=
                                transform->Rotation * (transform->Scale * modelDelta);
                        }
                    }
                    else
                    {
                        driveEntities.push_back(entity);
                        driveDeltas.push_back(modelDelta);
                    }
                }
            }

            skeleton.ComputeSkinningMatrices(localPose, pose->Skinning);
        }

        // Publish Drive-mode deltas now that iteration is done; add a RootMotionDelta on first run.
        for (usize i = 0; i < driveEntities.size(); ++i)
        {
            const Entity entity = driveEntities[i];
            if (!scene.Has<RootMotionDelta>(entity))
            {
                scene.Add<RootMotionDelta>(entity, RootMotionDelta{});
            }
            scene.Get<RootMotionDelta>(entity).Translation = driveDeltas[i];
        }
    }
}
