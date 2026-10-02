#include <Veng/Scene/AnimationSystem.h>

#include <algorithm>
#include <bit>
#include <cmath>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Scene/AnimationBlend.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    namespace
    {
        // The index of the key starting the span containing t, for keys.front().Time < t <
        // keys.back().Time: the last key at or before t.
        template <class Key>
        usize FindKeySpan(const vector<Key>& keys, const f32 t)
        {
            const auto next = std::ranges::upper_bound(keys, t, {}, &Key::Time);
            return static_cast<usize>(next - keys.begin()) - 1;
        }

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
            const usize i = FindKeySpan(keys, t);
            const f32 span = keys[i + 1].Time - keys[i].Time;
            const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
            return glm::mix(keys[i].Value, keys[i + 1].Value, alpha);
        }

        // A playback time wrapped into [0, duration) when looping or clamped to [0, duration]
        // otherwise; a clip with no duration samples the time as given.
        f32 ResolveClipTime(const f32 duration, const f32 time, const bool loop)
        {
            if (duration <= 0.0f)
            {
                return time;
            }
            f32 t = loop ? std::fmod(time, duration) : glm::clamp(time, 0.0f, duration);
            if (t < 0.0f)
            {
                t += duration;
            }
            return t;
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
            const auto resolve = [&](const f32 t) -> f32
            { return duration <= 0.0f ? 0.0f : ResolveClipTime(duration, t, loop); };

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
            const usize i = FindKeySpan(keys, t);
            const f32 span = keys[i + 1].Time - keys[i].Time;
            const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
            return glm::slerp(keys[i].Value, keys[i + 1].Value, alpha);
        }

        // Samples a clip at an already-resolved time into per-bone local TRS.
        void SampleLocalPoseAt(const Skeleton& skeleton, const Animation& animation, const f32 t,
                               vector<JointPose>& out)
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

        const f32 t = ResolveClipTime(animation.Duration, time, loop);
        for (const AnimationChannel& channel : animation.Channels)
        {
            if (channel.BoneIndex >= count)
            {
                continue;
            }
            const Bone& bone = skeleton.Bones[channel.BoneIndex];
            out[channel.BoneIndex] =
                ComposeBoneTransform(SampleVec3(channel.Position, t, bone.LocalPosition),
                                     SampleQuat(channel.Rotation, t, bone.LocalRotation),
                                     SampleVec3(channel.Scale, t, bone.LocalScale));
        }
    }

    i32 FindRootMotionBone(const Skeleton& skeleton, const Animation& animation)
    {
        const i32 cached = animation.RootMotionBone;
        if (cached != RootMotionBoneUnknown && cached < static_cast<i32>(skeleton.Bones.size()))
        {
            return cached;
        }
        return FindAnimatedRootBone(animation, skeleton.Bones.size());
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
        SampleLocalPoseAt(skeleton, animation, ResolveClipTime(animation.Duration, time, loop),
                          out);
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
            out[i] = ComposeBoneTransform(pose[i].Translation, pose[i].Rotation, pose[i].Scale);
        }
    }

    void ApplyJointRotations(const std::span<const JointRotation> rotations,
                             vector<JointPose>& pose)
    {
        for (const JointRotation& applied : rotations)
        {
            if (applied.Joint < pose.size())
            {
                pose[applied.Joint].Rotation = pose[applied.Joint].Rotation * applied.Rotation;
            }
        }
    }

    void ResolveJointOverrides(const Skeleton& skeleton, JointOverrides& overrides,
                               vector<JointRotation>& out)
    {
        const bool stale =
            overrides.ResolvedSkeleton != &skeleton ||
            overrides.Resolved.size() != overrides.Entries.size() ||
            !std::ranges::equal(overrides.Resolved, overrides.Entries, {},
                                &ResolvedJointOverride::Joint, &JointOverride::Joint);
        if (stale)
        {
            overrides.Resolved.clear();
            overrides.Resolved.reserve(overrides.Entries.size());
            for (const JointOverride& entry : overrides.Entries)
            {
                const i32 index = skeleton.FindBone(entry.Joint);
                if (index < 0)
                {
                    Log::Warn("JointOverrides: skeleton has no joint named '{}'; ignored",
                              entry.Joint);
                }
                overrides.Resolved.push_back(
                    ResolvedJointOverride{.Joint = entry.Joint, .Index = index});
            }
            overrides.ResolvedSkeleton = &skeleton;
        }

        out.clear();
        for (usize i = 0; i < overrides.Entries.size(); ++i)
        {
            const i32 index = overrides.Resolved[i].Index;
            if (index >= 0)
            {
                out.push_back(JointRotation{.Joint = static_cast<usize>(index),
                                            .Rotation = overrides.Entries[i].LocalRotation});
            }
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

        // Whether an entity draws a resident skinned mesh whose skeleton is loaded.
        const Skeleton* ResidentSkeleton(const Scene& scene, const Entity entity)
        {
            const auto* renderer = scene.TryGet<MeshRenderer>(entity);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded() || !renderer->Mesh->IsSkinned())
            {
                return nullptr;
            }
            const AssetHandle<Skeleton>& skeleton = renderer->Mesh->GetSkeleton();
            return skeleton.IsLoaded() ? skeleton.Get() : nullptr;
        }
        // One clip sampled at an effective (looped or clamped) time; a null clip stands for the base.
        struct ClipSample
        {
            const Animation* Clip = nullptr;
            f32 Time = 0.0f;
        };

        // How the final pose relates to the base: the base itself, one clip, or a crossfade.
        enum class PoseLayer : u8
        {
            Base,
            Single,
            Crossfade,
        };

        // The inputs a pose composes, in a canonical form: an input the pose does not read is
        // zeroed, so two frames differing only in an unread input key identically.
        struct PoseRecipe
        {
            // The base pose: bind with no clip, BaseLo alone without BaseHi, else their blend.
            ClipSample BaseLo;
            ClipSample BaseHi;
            f32 BaseWeight = 0.0f;
            // The final pose over the base: To alone, or From crossfading to To.
            PoseLayer Layer = PoseLayer::Base;
            ClipSample From;
            ClipSample To;
            f32 Transition = 0.0f;
            // The bone held at its bind translation, or -1.
            i32 RootBone = -1;
        };

        bool ReadsBase(const PoseRecipe& recipe)
        {
            switch (recipe.Layer)
            {
            case PoseLayer::Base:
                return true;
            case PoseLayer::Single:
                return recipe.To.Clip == nullptr;
            case PoseLayer::Crossfade:
                return recipe.From.Clip == nullptr || recipe.To.Clip == nullptr;
            }
            return true;
        }

        void Canonicalize(PoseRecipe& recipe)
        {
            if (recipe.Layer == PoseLayer::Single && recipe.To.Clip == nullptr)
            {
                recipe.Layer = PoseLayer::Base;
            }
            if (recipe.Layer != PoseLayer::Crossfade)
            {
                recipe.From = {};
                recipe.Transition = 0.0f;
            }
            if (recipe.Layer == PoseLayer::Base)
            {
                recipe.To = {};
            }
            if (!ReadsBase(recipe) || recipe.BaseLo.Clip == nullptr)
            {
                recipe.BaseLo = {};
                recipe.BaseHi = {};
            }
            if (recipe.BaseHi.Clip == nullptr)
            {
                recipe.BaseWeight = 0.0f;
            }
            for (ClipSample* sample : {&recipe.BaseLo, &recipe.BaseHi, &recipe.From, &recipe.To})
            {
                if (sample->Clip == nullptr)
                {
                    sample->Time = 0.0f;
                }
            }
        }

        u64 Bits(const f32 value)
        {
            return std::bit_cast<u32>(value);
        }

        u64 Bits(const void* pointer)
        {
            return reinterpret_cast<std::uintptr_t>(pointer);
        }

        // Encodes everything the palette is a function of into a fixed-layout word stream, so equal
        // streams pose identically. Floats compare by bit pattern: an identical rewrite matches.
        void EncodeKey(const Skeleton& skeleton, const PoseRecipe& recipe,
                       const std::span<const JointRotation> rotations, vector<u64>& out)
        {
            out.clear();
            out.push_back(Bits(&skeleton));
            for (const ClipSample& sample : {recipe.BaseLo, recipe.BaseHi, recipe.From, recipe.To})
            {
                out.push_back(Bits(sample.Clip));
                out.push_back(Bits(sample.Time));
            }
            out.push_back(Bits(recipe.BaseWeight));
            out.push_back(Bits(recipe.Transition));
            out.push_back(static_cast<u64>(recipe.Layer));
            out.push_back(static_cast<u64>(static_cast<i64>(recipe.RootBone)));
            out.push_back(rotations.size());
            for (const JointRotation& rotation : rotations)
            {
                out.push_back(rotation.Joint);
                out.push_back(Bits(rotation.Rotation.x) | (Bits(rotation.Rotation.y) << 32U));
                out.push_back(Bits(rotation.Rotation.z) | (Bits(rotation.Rotation.w) << 32U));
            }
        }

        // A source a crossfade or state reads: the named state's clip at its effective time, or
        // the base when the name is empty, unknown, or its clip is not resident.
        ClipSample StateSource(const AnimationStateSet& set, const string& name, const f32 time)
        {
            const AnimationState* state = FindState(set, name);
            if (state == nullptr || !state->Clip.IsLoaded())
            {
                return {};
            }
            const Animation* clip = state->Clip.Get();
            return {.Clip = clip, .Time = ResolveClipTime(clip->Duration, time, state->Loop)};
        }

        // Advances a blend/state entity's clocks and reduces it to the recipe its pose composes:
        // the phase-synced blend as the base, an optional named state crossfaded over it, and the
        // baked root translation stripped (the controller owns position).
        PoseRecipe AdvanceBlended(const Skeleton& skeleton, const Animator& animator,
                                  AnimationBlend* blend, AnimationStateSet* stateSet,
                                  const f32 delta, vector<f32>& thresholds)
        {
            const bool playing = animator.Playing;
            PoseRecipe recipe;

            if (blend != nullptr && !blend->Samples.empty())
            {
                thresholds.clear();
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

                // Each bracket clip samples at the shared phase of its own duration.
                const auto atPhase = [&](const AssetHandle<Animation>& handle) -> ClipSample
                {
                    const Animation* clip = handle.Get();
                    return {.Clip = clip,
                            .Time = ResolveClipTime(clip->Duration, blend->Phase * clip->Duration,
                                                    true)};
                };
                const AssetHandle<Animation>& lo = blend->Samples[bracket.Lo].Clip;
                const AssetHandle<Animation>& hi = blend->Samples[bracket.Hi].Clip;
                const bool loLoaded = lo.IsLoaded();
                const bool hiLoaded = hi.IsLoaded();
                if (loLoaded && hiLoaded && bracket.Lo != bracket.Hi)
                {
                    recipe.BaseLo = atPhase(lo);
                    recipe.BaseHi = atPhase(hi);
                    recipe.BaseWeight = bracket.Weight;
                }
                else if (loLoaded || hiLoaded)
                {
                    recipe.BaseLo = atPhase(loLoaded ? lo : hi);
                }
            }

            if (stateSet != nullptr)
            {
                UpdateStateSet(*stateSet, delta * animator.Speed, delta, playing);
                const ClipSample current =
                    StateSource(*stateSet, stateSet->CurrentState, stateSet->CurrentTime);
                if (stateSet->Transition >= 1.0f ||
                    stateSet->PreviousState == stateSet->CurrentState)
                {
                    recipe.Layer = PoseLayer::Single;
                    recipe.To = current;
                }
                else
                {
                    recipe.Layer = PoseLayer::Crossfade;
                    recipe.From =
                        StateSource(*stateSet, stateSet->PreviousState, stateSet->PreviousTime);
                    recipe.To = current;
                    recipe.Transition = stateSet->Transition;
                }
            }

            recipe.RootBone = RepresentativeRootBone(skeleton, blend, stateSet);
            return recipe;
        }
    }

    struct AnimationSystem::Workspace
    {
        vector<Entity> NeedPose;
        vector<Entity> DriveEntities;
        vector<vec3> DriveDeltas;
        vector<JointRotation> Rotations;
        vector<f32> Thresholds;
        vector<u64> Key;
        vector<JointPose> Base;
        vector<JointPose> PoseA;
        vector<JointPose> PoseB;
        vector<JointPose> Final;
        vector<mat4> Local;
        vector<mat4> Model;

        void Sample(const Skeleton& skeleton, const ClipSample& sample, vector<JointPose>& out)
        {
            SampleLocalPoseAt(skeleton, *sample.Clip, sample.Time, out);
        }

        // Writes the recipe's base pose into Base; uses PoseA/PoseB as scratch.
        void EvaluateBase(const Skeleton& skeleton, const PoseRecipe& recipe)
        {
            if (recipe.BaseLo.Clip == nullptr)
            {
                BindLocalPose(skeleton, Base);
            }
            else if (recipe.BaseHi.Clip == nullptr)
            {
                Sample(skeleton, recipe.BaseLo, Base);
            }
            else
            {
                Sample(skeleton, recipe.BaseLo, PoseA);
                Sample(skeleton, recipe.BaseHi, PoseB);
                BlendLocalPoses(PoseA, PoseB, recipe.BaseWeight, Base);
            }
        }

        // Writes the recipe's local pose, root stripped, into Final.
        void Evaluate(const Skeleton& skeleton, const PoseRecipe& recipe)
        {
            if (ReadsBase(recipe))
            {
                EvaluateBase(skeleton, recipe);
            }
            switch (recipe.Layer)
            {
            case PoseLayer::Base:
                std::swap(Final, Base);
                break;
            case PoseLayer::Single:
                Sample(skeleton, recipe.To, Final);
                break;
            case PoseLayer::Crossfade:
                if (recipe.From.Clip != nullptr)
                {
                    Sample(skeleton, recipe.From, PoseA);
                }
                else
                {
                    PoseA = Base;
                }
                if (recipe.To.Clip != nullptr)
                {
                    Sample(skeleton, recipe.To, PoseB);
                }
                else
                {
                    PoseB = Base;
                }
                BlendLocalPoses(PoseA, PoseB, recipe.Transition, Final);
                break;
            }

            const i32 root = recipe.RootBone;
            if (root >= 0 && static_cast<usize>(root) < Final.size())
            {
                Final[static_cast<usize>(root)].Translation =
                    skeleton.Bones[static_cast<usize>(root)].LocalPosition;
            }
        }

        // Re-poses into pose unless its inputs are the ones it was last computed from.
        void Pose(const Skeleton& skeleton, PoseRecipe recipe, SkinnedPose& pose)
        {
            Canonicalize(recipe);
            EncodeKey(skeleton, recipe, Rotations, Key);
            if (Key == pose.InputKey && pose.Skinning.size() == skeleton.Bones.size())
            {
                return;
            }

            Evaluate(skeleton, recipe);
            ApplyJointRotations(Rotations, Final);
            ComposeLocalPose(Final, Local);
            skeleton.ComputeSkinningMatrices(Local, pose.Skinning, Model);
            std::swap(pose.InputKey, Key);
            ++pose.Version;
        }
    };

    AnimationSystem::AnimationSystem() : m_Workspace(CreateUnique<Workspace>()) {}

    AnimationSystem::~AnimationSystem() = default;

    void AnimationSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& /*context*/)
    {
        const Scene& readScene = scene;
        Workspace& work = *m_Workspace;

        // Add a SkinnedPose to any animated, resident, skinned-mesh entity that lacks one.
        // Collected first so the structural add never happens mid-iteration.
        {
            VE_PROFILE_SCOPE("Animation/Collect");
            work.NeedPose.clear();
            const auto collect = [&](const Entity entity)
            {
                if (scene.Has<SkinnedPose>(entity))
                {
                    return;
                }
                const auto* renderer = readScene.TryGet<MeshRenderer>(entity);
                if (renderer != nullptr && renderer->Mesh.IsLoaded() && renderer->Mesh->IsSkinned())
                {
                    work.NeedPose.push_back(entity);
                }
            };
            for (auto [entity, animator] : readScene.View<Animator>())
            {
                collect(entity);
            }
            for (auto [entity, overrides] : readScene.View<JointOverrides>())
            {
                if (!scene.Has<Animator>(entity))
                {
                    collect(entity);
                }
            }
            for (const Entity entity : work.NeedPose)
            {
                scene.Add<SkinnedPose>(entity, SkinnedPose{});
            }
        }

        // Advance each animator and write its skinning palette. MeshRenderer is read through the
        // const scene so this View does not bump the spatial version (no broadphase rebuild).
        // Drive-mode root-motion deltas are collected and published after the loop so the
        // RootMotionDelta add never happens mid-iteration.
        work.DriveEntities.clear();
        work.DriveDeltas.clear();
        {
            VE_PROFILE_SCOPE("Animation/Animators");
            for (auto [entity, animator] : scene.View<Animator>())
            {
                auto* pose = scene.TryGet<SkinnedPose>(entity);
                if (pose == nullptr)
                {
                    continue;
                }

                const Skeleton* resident = ResidentSkeleton(readScene, entity);
                if (resident == nullptr)
                {
                    continue;
                }
                const Skeleton& skeleton = *resident;

                work.Rotations.clear();
                if (auto* overrides = scene.TryGet<JointOverrides>(entity))
                {
                    ResolveJointOverrides(skeleton, *overrides, work.Rotations);
                }

                // A blend space or state set replaces the single-clip play: pose in blend/state space
                // into the same SkinnedPose. An Animator carrying neither is the single-clip path below.
                auto* blend = scene.TryGet<AnimationBlend>(entity);
                auto* stateSet = scene.TryGet<AnimationStateSet>(entity);
                if (blend != nullptr || stateSet != nullptr)
                {
                    VE_PROFILE_SCOPE("Animation/Blended");
                    work.Pose(
                        skeleton,
                        AdvanceBlended(skeleton, animator, blend, stateSet, delta, work.Thresholds),
                        *pose);
                    continue;
                }

                const f32 prevTime = animator.Time;
                if (animator.Playing)
                {
                    animator.Time += delta * animator.Speed;
                }

                PoseRecipe recipe;
                if (animator.Clip.IsLoaded())
                {
                    const Animation& clip = *animator.Clip.Get();
                    recipe.BaseLo = {
                        .Clip = &clip,
                        .Time = ResolveClipTime(clip.Duration, animator.Time, animator.Loop)};

                    // The rendered pose holds the root bone at its bind position; the extracted
                    // translation is discarded, applied to the Transform, or published.
                    const i32 rootBone = FindRootMotionBone(skeleton, clip);
                    if (rootBone >= 0 && static_cast<usize>(rootBone) < skeleton.Bones.size())
                    {
                        recipe.RootBone = rootBone;
                        if (animator.RootMotion != RootMotionMode::Discard)
                        {
                            const vec3 localDelta = ExtractRootDelta(
                                skeleton, clip, rootBone, prevTime, animator.Time, animator.Loop);
                            const vec3 modelDelta =
                                BindModelRotation(
                                    skeleton, skeleton.Bones[static_cast<usize>(rootBone)].Parent) *
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
                                work.DriveEntities.push_back(entity);
                                work.DriveDeltas.push_back(modelDelta);
                            }
                        }
                    }
                }

                work.Pose(skeleton, recipe, *pose);
            }
        }

        // A clip-less entity's JointOverrides pose its skeleton from the bind pose.
        {
            VE_PROFILE_SCOPE("Animation/OverridesOnly");
            for (auto [entity, overrides] : scene.View<JointOverrides>())
            {
                if (scene.Has<Animator>(entity))
                {
                    continue;
                }
                auto* pose = scene.TryGet<SkinnedPose>(entity);
                const Skeleton* skeleton = ResidentSkeleton(readScene, entity);
                if (pose == nullptr || skeleton == nullptr)
                {
                    continue;
                }
                ResolveJointOverrides(*skeleton, overrides, work.Rotations);
                work.Pose(*skeleton, PoseRecipe{}, *pose);
            }
        }

        // Publish Drive-mode deltas now that iteration is done; add a RootMotionDelta on first run.
        for (usize i = 0; i < work.DriveEntities.size(); ++i)
        {
            const Entity entity = work.DriveEntities[i];
            if (!scene.Has<RootMotionDelta>(entity))
            {
                scene.Add<RootMotionDelta>(entity, RootMotionDelta{});
            }
            scene.Get<RootMotionDelta>(entity).Translation = work.DriveDeltas[i];
        }
    }
}
