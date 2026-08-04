#include <Veng/Audio/AudioSystem.h>

#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <vector>

namespace Veng::Audio
{
    f32 DistanceAttenuation(const f32 distance, const f32 minDistance, const f32 maxDistance)
    {
        const f32 lo = std::max(minDistance, 0.0f);
        const f32 hi = std::max(maxDistance, lo);
        if (distance <= lo)
        {
            return 1.0f;
        }
        if (distance >= hi || hi <= lo)
        {
            return distance <= lo ? 1.0f : 0.0f;
        }
        return (hi - distance) / (hi - lo);
    }

    f32 StereoPan(const vec3 listenerPosition, const quat listenerRotation,
                  const vec3 sourcePosition)
    {
        const vec3 toSource = sourcePosition - listenerPosition;
        if (glm::dot(toSource, toSource) < 1e-12f)
        {
            return 0.0f;
        }
        const vec3 direction = glm::normalize(toSource);
        const vec3 right = listenerRotation * vec3(1.0f, 0.0f, 0.0f);
        return std::clamp(glm::dot(direction, right), -1.0f, 1.0f);
    }

    f32 DopplerRatio(const vec3 listenerPosition, const vec3 listenerVelocity,
                     const vec3 sourcePosition, const vec3 sourceVelocity, const f32 speedOfSound)
    {
        const vec3 toSource = sourcePosition - listenerPosition;
        const f32 distanceSq = glm::dot(toSource, toSource);
        if (distanceSq < 1e-12f || speedOfSound <= 0.0f)
        {
            return 1.0f;
        }
        const vec3 direction = toSource / std::sqrt(distanceSq);

        // Radial components along the listener→source line: the listener closing on the source
        // raises pitch (numerator), the source receding from the listener lowers it (denominator).
        const f32 listenerRadial = glm::dot(listenerVelocity, direction);
        const f32 sourceRadial = glm::dot(sourceVelocity, direction);
        const f32 denominator = speedOfSound + sourceRadial;
        if (denominator <= 0.0f)
        {
            return 2.0f;
        }
        return std::clamp((speedOfSound + listenerRadial) / denominator, 0.5f, 2.0f);
    }

    f32 ReverbSend(const f32 distance, const f32 minDistance, const f32 maxDistance)
    {
        const f32 lo = std::max(minDistance, 0.0f);
        const f32 hi = std::max(maxDistance, lo);
        const f32 span = hi - lo;
        if (span <= 0.0f)
        {
            return 0.0f;
        }
        return std::clamp((distance - lo) / span, 0.0f, 1.0f) * 0.5f;
    }
}

namespace Veng
{
    namespace
    {
        // The pose an entity is *drawn* at this frame — the interpolated blend between the last two
        // Sim ticks, the same source the renderer gathers from — so a sound sits where its emitter is
        // drawn rather than a partial tick ahead. Mirrors CameraRig's DrawnTargetPose.
        mat4 DrawnPose(const Scene& scene, const Entity entity, const f32 alpha)
        {
            if (alpha > 0.0f && scene.HasTransformInterpolation())
            {
                return scene.GetInterpolatedWorldTransform(entity, alpha);
            }
            return WorldMatrix(scene, entity);
        }

        // Folds an AudioSource's authored fields and the resolved geometry into the final voice
        // parameters the mixer consumes — the numbers the unit tests pin. A non-spatial source routes
        // straight to its bus at gain with no attenuation or pan.
        Audio::VoiceParams Spatialize(const AudioSource& source, const vec3 sourcePosition,
                                      const vec3 sourceVelocity,
                                      const Audio::ListenerPose& listener)
        {
            Audio::VoiceParams params;
            params.Bus = source.Bus;
            params.Loop = source.Looping;

            if (!source.Spatial)
            {
                params.Gain = source.Gain * listener.Gain;
                params.Pitch = source.Pitch;
                return params;
            }

            const f32 distance = glm::length(sourcePosition - listener.Position);
            const f32 attenuation =
                Audio::DistanceAttenuation(distance, source.MinDistance, source.MaxDistance);
            params.Gain = source.Gain * listener.Gain * attenuation;
            params.Pan = Audio::StereoPan(listener.Position, listener.Rotation, sourcePosition);
            params.Pitch = source.Pitch * Audio::DopplerRatio(listener.Position, listener.Velocity,
                                                              sourcePosition, sourceVelocity,
                                                              Audio::DefaultSpeedOfSound);
            params.Occlusion = std::clamp(source.OcclusionFactor, 0.0f, 1.0f);
            params.ReverbSend = Audio::ReverbSend(distance, source.MinDistance, source.MaxDistance);
            return params;
        }
    }

    void AudioSystem::OnStart(Scene& scene, const SystemContext& context)
    {
        m_Voices.clear();
        m_Finished.clear();
        m_SourcePosition.clear();
        m_HasListenerPosition = false;

        // Hand any authored initial track to the music director once, at its authored fade — a level
        // with no MusicState simply starts silent on the Music bus.
        if (const MusicState* music = scene.TryGetFirst<MusicState>(); music != nullptr)
        {
            context.Audio.Music().Set(
                music->Track,
                Audio::MusicTransition{.FadeSeconds = music->FadeSeconds, .Loop = music->Loop});
        }
    }

    void AudioSystem::OnStop(Scene& /*scene*/, const SystemContext& context)
    {
        for (const auto& [entity, voice] : m_Voices)
        {
            context.Audio.StopVoice(voice);
        }
        m_Voices.clear();
        m_Finished.clear();
        m_SourcePosition.clear();
        m_HasListenerPosition = false;
    }

    void AudioSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        Audio::AudioEngine& engine = context.Audio;
        const f32 alpha = context.Alpha;

        // Resolve the single listener from its live drawn pose, and difference its position for
        // velocity. No listener leaves the pose at the origin so non-spatial sound still plays.
        Audio::ListenerPose listener;
        Entity listenerEntity = Entity::Null;
        scene.Each<Transform, AudioListener>(
            [&](const Entity entity, Transform&, AudioListener& component)
            {
                if (listenerEntity != Entity::Null)
                {
                    return;
                }
                listenerEntity = entity;
                const mat4 world = DrawnPose(scene, entity, alpha);
                listener.Position = vec3(world[3]);
                listener.Rotation = glm::quat_cast(mat3(world));
                listener.Gain = component.Gain;
            });
        if (listenerEntity != Entity::Null && m_HasListenerPosition && delta > 0.0f)
        {
            listener.Velocity = (listener.Position - m_ListenerPosition) / delta;
        }
        m_ListenerPosition = listener.Position;
        m_HasListenerPosition = listenerEntity != Entity::Null;

        // Drop voices the device retired (surfaced through IsVoiceLive once Pump drained the
        // retired-voice channel): a finished non-looping source stays finished and is not restarted.
        for (auto it = m_Voices.begin(); it != m_Voices.end();)
        {
            if (engine.IsVoiceLive(it->second))
            {
                ++it;
                continue;
            }
            const AudioSource* source = scene.TryGet<AudioSource>(it->first);
            if (source != nullptr && !source->Looping)
            {
                m_Finished.insert(it->first);
            }
            it = m_Voices.erase(it);
        }

        // Gather the sources that should be sounding this tick, with their spatialized parameters
        // and post-attenuation loudness (the cap's priority key).
        struct Candidate
        {
            Entity Source;
            AssetHandle<Audio::AudioClip> Clip;
            Audio::VoiceParams Params;
        };
        std::vector<Candidate> candidates;
        std::unordered_set<Entity> present;

        scene.Each<Transform, AudioSource>(
            [&](const Entity entity, Transform&, AudioSource& source)
            {
                present.insert(entity);
                if (m_Finished.contains(entity))
                {
                    return;
                }

                // Only a PlayOnStart source (or one already sounding) plays here; a code-triggered
                // one-shot is a later capability.
                const bool playing = m_Voices.contains(entity);
                if (!source.PlayOnStart && !playing)
                {
                    return;
                }

                // A Pcm clip plays off its resident buffer, an Encoded clip through the streaming
                // path; only an unresident clip (neither resident nor encoded) is silent.
                const Audio::AudioClip* clip = source.Clip.Get();
                if (clip == nullptr ||
                    (clip->Buffer() == nullptr && clip->Storage() != Audio::AudioStorage::Encoded))
                {
                    return;
                }

                const mat4 world = DrawnPose(scene, entity, alpha);
                const vec3 position = vec3(world[3]);
                vec3 velocity(0.0f);
                if (const auto it = m_SourcePosition.find(entity);
                    it != m_SourcePosition.end() && delta > 0.0f)
                {
                    velocity = (position - it->second) / delta;
                }
                m_SourcePosition[entity] = position;

                candidates.push_back(Candidate{
                    .Source = entity,
                    .Clip = source.Clip,
                    .Params = Spatialize(source, position, velocity, listener),
                });
            });

        // Stop voices whose source vanished, and forget bookkeeping for entities no longer present.
        for (auto it = m_Voices.begin(); it != m_Voices.end();)
        {
            if (present.contains(it->first))
            {
                ++it;
                continue;
            }
            engine.StopVoice(it->second);
            it = m_Voices.erase(it);
        }
        std::erase_if(m_SourcePosition,
                      [&](const auto& entry) { return !present.contains(entry.first); });
        std::erase_if(m_Finished, [&](const Entity entity) { return !present.contains(entity); });

        // Cap: keep the loudest-after-attenuation, stopping the voices of the dropped sources.
        if (candidates.size() > m_VoiceCap)
        {
            std::partial_sort(candidates.begin(),
                              candidates.begin() + static_cast<std::ptrdiff_t>(m_VoiceCap),
                              candidates.end(), [](const Candidate& a, const Candidate& b)
                              { return a.Params.Gain > b.Params.Gain; });
            for (usize i = m_VoiceCap; i < candidates.size(); ++i)
            {
                if (const auto it = m_Voices.find(candidates[i].Source); it != m_Voices.end())
                {
                    engine.StopVoice(it->second);
                    m_Voices.erase(it);
                }
            }
            candidates.resize(m_VoiceCap);
        }

        // Start the new voices and retune the live ones.
        for (const Candidate& candidate : candidates)
        {
            const auto it = m_Voices.find(candidate.Source);
            if (it != m_Voices.end())
            {
                engine.SetVoiceParams(it->second, candidate.Params);
                continue;
            }
            const Audio::VoiceHandle voice = engine.AddClipVoice(candidate.Clip, candidate.Params);
            if (voice.IsValid())
            {
                m_Voices[candidate.Source] = voice;
            }
        }

        // Merge the engine's code-triggered voices into the same snapshot: advance the music
        // crossfade and re-spatialize every PlayAt voice against this frame's listener.
        engine.UpdateManagedVoices(listener, delta);
    }

    optional<vec3> AudioSystem::GetDebugSourcePosition(const Entity entity) const
    {
        const auto it = m_SourcePosition.find(entity);
        if (it == m_SourcePosition.end())
        {
            return std::nullopt;
        }
        return it->second;
    }
}
