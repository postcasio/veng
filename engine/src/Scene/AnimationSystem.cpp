#include <Veng/Scene/AnimationSystem.h>

#include <cmath>

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/AnimationBlend.h>
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

    BlendBracket FindBlendBracket(const std::span<const f32> thresholds, const f32 parameter)
    {
        const usize count = thresholds.size();
        if (count == 0)
        {
            return {};
        }
        if (parameter <= thresholds.front())
        {
            return {.Lo = 0, .Hi = 0, .Weight = 0.0f};
        }
        if (parameter >= thresholds.back())
        {
            return {.Lo = count - 1, .Hi = count - 1, .Weight = 0.0f};
        }
        for (usize i = 0; i + 1 < count; ++i)
        {
            if (parameter < thresholds[i + 1])
            {
                const f32 span = thresholds[i + 1] - thresholds[i];
                const f32 weight = span > 0.0f ? (parameter - thresholds[i]) / span : 0.0f;
                return {.Lo = i, .Hi = i + 1, .Weight = weight};
            }
        }
        return {.Lo = count - 1, .Hi = count - 1, .Weight = 0.0f};
    }

    f32 AdvanceCrossfade(const f32 weight, const f32 fadeIn, const f32 delta)
    {
        if (fadeIn <= 0.0f)
        {
            return 1.0f;
        }
        return glm::clamp(weight + delta / fadeIn, 0.0f, 1.0f);
    }

    void SampleAnimationLocalPose(const Skeleton& skeleton, const Animation& animation,
                                  const f32 time, const bool loop, vector<JointPose>& out)
    {
        const usize count = skeleton.Bones.size();
        out.resize(count);
        for (usize i = 0; i < count; ++i)
        {
            const Bone& bone = skeleton.Bones[i];
            out[i] = JointPose{.Translation = bone.LocalPosition,
                               .Rotation = bone.LocalRotation,
                               .Scale = bone.LocalScale};
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
            out[channel.BoneIndex] =
                JointPose{.Translation = SampleVec3(channel.Position, t, bone.LocalPosition),
                          .Rotation = SampleQuat(channel.Rotation, t, bone.LocalRotation),
                          .Scale = SampleVec3(channel.Scale, t, bone.LocalScale)};
        }
    }

    void BlendLocalPoses(const vector<JointPose>& a, const vector<JointPose>& b, const f32 weight,
                         vector<JointPose>& out)
    {
        const usize count = a.size();
        out.resize(count);
        for (usize i = 0; i < count; ++i)
        {
            // Flip the second quaternion into the first's hemisphere so slerp takes the short arc.
            quat rb = b[i].Rotation;
            if (glm::dot(a[i].Rotation, rb) < 0.0f)
            {
                rb = -rb;
            }
            out[i] = JointPose{.Translation = glm::mix(a[i].Translation, b[i].Translation, weight),
                               .Rotation = glm::slerp(a[i].Rotation, rb, weight),
                               .Scale = glm::mix(a[i].Scale, b[i].Scale, weight)};
        }
    }

    void ComposeLocalPose(const vector<JointPose>& pose, vector<mat4>& out)
    {
        out.resize(pose.size());
        for (usize i = 0; i < pose.size(); ++i)
        {
            out[i] = glm::translate(mat4(1.0f), pose[i].Translation) *
                     glm::mat4_cast(pose[i].Rotation) * glm::scale(mat4(1.0f), pose[i].Scale);
        }
    }

    namespace
    {
        // Fills out with the skeleton's bind-pose local TRS — the base an empty blend falls back to.
        void BindLocalPose(const Skeleton& skeleton, vector<JointPose>& out)
        {
            out.resize(skeleton.Bones.size());
            for (usize i = 0; i < skeleton.Bones.size(); ++i)
            {
                const Bone& bone = skeleton.Bones[i];
                out[i] = JointPose{.Translation = bone.LocalPosition,
                                   .Rotation = bone.LocalRotation,
                                   .Scale = bone.LocalScale};
            }
        }

        // The blended clip duration the shared phase advances against, so the effective playback
        // cadence matches the pace being blended toward.
        f32 BlendReferenceDuration(const AnimationBlend& blend, const BlendBracket& bracket)
        {
            const auto duration = [&](const usize index) -> f32
            {
                const AssetHandle<Animation>& clip = blend.Samples[index].Clip;
                return clip.IsLoaded() ? clip.Get()->Duration : 0.0f;
            };
            const f32 lo = duration(bracket.Lo);
            const f32 hi = duration(bracket.Hi);
            if (bracket.Lo == bracket.Hi || hi <= 0.0f)
            {
                return lo;
            }
            if (lo <= 0.0f)
            {
                return hi;
            }
            return glm::mix(lo, hi, bracket.Weight);
        }

        // Samples the bracketed blend at the shared phase (each clip at phase * its own duration) and
        // blends by the bracket weight. False when neither bracket clip is resident.
        bool SampleBlendPose(const Skeleton& skeleton, const AnimationBlend& blend,
                             const BlendBracket& bracket, const f32 phase, vector<JointPose>& out)
        {
            const AssetHandle<Animation>& lo = blend.Samples[bracket.Lo].Clip;
            const AssetHandle<Animation>& hi = blend.Samples[bracket.Hi].Clip;
            const bool loLoaded = lo.IsLoaded();
            const bool hiLoaded = hi.IsLoaded();
            if (!loLoaded && !hiLoaded)
            {
                return false;
            }

            if (loLoaded && hiLoaded && bracket.Lo != bracket.Hi)
            {
                vector<JointPose> poseLo;
                vector<JointPose> poseHi;
                SampleAnimationLocalPose(skeleton, *lo.Get(), phase * lo.Get()->Duration, true,
                                         poseLo);
                SampleAnimationLocalPose(skeleton, *hi.Get(), phase * hi.Get()->Duration, true,
                                         poseHi);
                BlendLocalPoses(poseLo, poseHi, bracket.Weight, out);
                return true;
            }

            const AssetHandle<Animation>& only = loLoaded ? lo : hi;
            SampleAnimationLocalPose(skeleton, *only.Get(), phase * only.Get()->Duration, true,
                                     out);
            return true;
        }

        // Finds the named state, or nullptr for an empty/unknown name.
        const AnimationState* FindState(const AnimationStateSet& set, const string& name)
        {
            if (name.empty())
            {
                return nullptr;
            }
            for (const AnimationState& state : set.States)
            {
                if (state.Name == name)
                {
                    return &state;
                }
            }
            return nullptr;
        }

        // Evaluates one crossfade source: the empty name is the base (blend) pose, a state name is
        // its clip sampled at the given time (falling back to the base when the clip is not resident).
        void EvalStateSource(const Skeleton& skeleton, const AnimationStateSet& set,
                             const string& name, const f32 time, const vector<JointPose>& base,
                             vector<JointPose>& out)
        {
            const AnimationState* state = FindState(set, name);
            if (state == nullptr || !state->Clip.IsLoaded())
            {
                out = base;
                return;
            }
            SampleAnimationLocalPose(skeleton, *state->Clip.Get(), time, state->Loop, out);
        }

        // Advances the state set's crossfade machine: honoring the requested state, snapping a new
        // request into a fresh transition, and ramping the crossfade + clip clocks.
        void UpdateStateSet(AnimationStateSet& set, const f32 clipDelta, const f32 fadeDelta,
                            const bool playing)
        {
            // An unknown requested name resolves to the blend, exactly like an empty one.
            const string request =
                FindState(set, set.RequestedState) != nullptr ? set.RequestedState : string();
            if (request != set.CurrentState)
            {
                set.PreviousState = set.CurrentState;
                set.PreviousTime = set.CurrentTime;
                set.CurrentState = request;
                set.CurrentTime = 0.0f;
                set.Transition = 0.0f;
            }

            const string& fadeName =
                set.CurrentState.empty() ? set.PreviousState : set.CurrentState;
            const AnimationState* fadeState = FindState(set, fadeName);
            const f32 fadeIn = fadeState != nullptr ? fadeState->FadeIn : 0.0f;

            if (playing)
            {
                set.Transition = AdvanceCrossfade(set.Transition, fadeIn, fadeDelta);
                if (!set.CurrentState.empty())
                {
                    set.CurrentTime += clipDelta;
                }
                if (!set.PreviousState.empty() && set.Transition < 1.0f)
                {
                    set.PreviousTime += clipDelta;
                }
            }
        }

        // The bone whose baked translation is stripped in the blend/state path: the first root-motion
        // bone any participating clip resolves to (all share the skeleton). -1 when none bake motion.
        i32 RepresentativeRootBone(const Skeleton& skeleton, const AnimationBlend* blend,
                                   const AnimationStateSet* set)
        {
            const auto tryClip = [&](const AssetHandle<Animation>& clip) -> i32
            { return clip.IsLoaded() ? FindRootMotionBone(skeleton, *clip.Get()) : -1; };

            if (blend != nullptr)
            {
                for (const BlendSample& sample : blend->Samples)
                {
                    const i32 bone = tryClip(sample.Clip);
                    if (bone >= 0)
                    {
                        return bone;
                    }
                }
            }
            if (set != nullptr)
            {
                for (const AnimationState& state : set->States)
                {
                    const i32 bone = tryClip(state.Clip);
                    if (bone >= 0)
                    {
                        return bone;
                    }
                }
            }
            return -1;
        }

        // Poses an entity carrying an AnimationBlend and/or AnimationStateSet into its SkinnedPose:
        // the phase-synced blend as the base, an optional named state crossfaded over it, and the
        // baked root translation stripped (the controller owns position).
        void PoseBlended(const Skeleton& skeleton, const Animator& animator, AnimationBlend* blend,
                         AnimationStateSet* stateSet, const f32 delta, SkinnedPose& pose)
        {
            const bool playing = animator.Playing;

            vector<JointPose> basePose;
            bool haveBase = false;
            if (blend != nullptr && !blend->Samples.empty())
            {
                vector<f32> thresholds;
                thresholds.reserve(blend->Samples.size());
                for (const BlendSample& sample : blend->Samples)
                {
                    thresholds.push_back(sample.Threshold);
                }
                const BlendBracket bracket = FindBlendBracket(thresholds, blend->Parameter);
                const f32 referenceDuration = BlendReferenceDuration(*blend, bracket);
                if (playing && referenceDuration > 0.0f)
                {
                    blend->Phase += delta * animator.Speed / referenceDuration;
                    blend->Phase -= std::floor(blend->Phase);
                }
                haveBase = SampleBlendPose(skeleton, *blend, bracket, blend->Phase, basePose);
            }
            if (!haveBase)
            {
                BindLocalPose(skeleton, basePose);
            }

            vector<JointPose> finalPose;
            if (stateSet != nullptr)
            {
                UpdateStateSet(*stateSet, delta * animator.Speed, delta, playing);
                if (stateSet->Transition >= 1.0f ||
                    stateSet->PreviousState == stateSet->CurrentState)
                {
                    EvalStateSource(skeleton, *stateSet, stateSet->CurrentState,
                                    stateSet->CurrentTime, basePose, finalPose);
                }
                else
                {
                    vector<JointPose> currentPose;
                    vector<JointPose> previousPose;
                    EvalStateSource(skeleton, *stateSet, stateSet->CurrentState,
                                    stateSet->CurrentTime, basePose, currentPose);
                    EvalStateSource(skeleton, *stateSet, stateSet->PreviousState,
                                    stateSet->PreviousTime, basePose, previousPose);
                    BlendLocalPoses(previousPose, currentPose, stateSet->Transition, finalPose);
                }
            }
            else
            {
                finalPose = std::move(basePose);
            }

            const i32 rootBone = RepresentativeRootBone(skeleton, blend, stateSet);
            if (rootBone >= 0 && static_cast<usize>(rootBone) < finalPose.size())
            {
                finalPose[static_cast<usize>(rootBone)].Translation =
                    skeleton.Bones[static_cast<usize>(rootBone)].LocalPosition;
            }

            vector<mat4> localPose;
            ComposeLocalPose(finalPose, localPose);
            skeleton.ComputeSkinningMatrices(localPose, pose.Skinning);
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

            // A blend space or state set replaces the single-clip play: pose in blend/state space
            // into the same SkinnedPose. An Animator carrying neither is the single-clip path below,
            // unchanged.
            auto* blend = scene.TryGet<AnimationBlend>(entity);
            auto* stateSet = scene.TryGet<AnimationStateSet>(entity);
            if (blend != nullptr || stateSet != nullptr)
            {
                PoseBlended(*skeletonHandle.Get(), animator, blend, stateSet, delta, *pose);
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
