#include <Veng/Scene/AnimationSystem.h>

#include <cmath>

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
            const AssetHandle<Skeleton>& skeleton = renderer->Mesh->GetSkeleton();
            if (!skeleton.IsLoaded())
            {
                continue;
            }

            if (animator.Playing)
            {
                animator.Time += delta * animator.Speed;
            }

            if (animator.Clip.IsLoaded())
            {
                vector<mat4> localPose;
                SampleAnimationPose(*skeleton.Get(), *animator.Clip.Get(), animator.Time,
                                    animator.Loop, localPose);
                skeleton->ComputeSkinningMatrices(localPose, pose->Skinning);
            }
            else
            {
                skeleton->ComputeBindPoseMatrices(pose->Skinning);
            }
        }
    }
}
