#include <Veng/Audio/AudioSystem.h>

#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/ScopedAudio.h>
#include <Veng/Log.h>
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
            // Intern the authored bus name to a BusId; the mixer resolves it against the active
            // graph at publish (a name the graph does not declare routes to Master with a warning).
            params.Bus = Audio::BusId{source.Bus};
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

        // The authored mix a positioned generator voice is spatialized from by the engine.
        Audio::SpatialVoiceMix MixOf(const AudioSource& source)
        {
            return Audio::SpatialVoiceMix{.Bus = Audio::BusId{source.Bus},
                                          .Gain = source.Gain,
                                          .Pitch = source.Pitch,
                                          .MinDistance = source.MinDistance,
                                          .MaxDistance = source.MaxDistance,
                                          .OcclusionFactor = source.OcclusionFactor};
        }
    }

    AudioSystem::VoiceShape AudioSystem::ShapeOf(const AudioSource& source)
    {
        if (source.Generator == nullptr)
        {
            return {};
        }
        return VoiceShape{
            .Generator = source.Generator.get(),
            .Spatial = source.Spatial,
            .Channels = source.Channels == 2 ? 2u : 1u,
            .Buffered = source.Buffered,
            .BufferSeconds = source.Buffered ? source.BufferSeconds : 0.0f,
        };
    }

    void AudioSystem::OnStart(Scene& /*scene*/, const SystemContext& /*context*/)
    {
        m_Voices.clear();
        m_Rejected.clear();
        m_SourcePosition.clear();
        m_HasListenerPosition = false;
    }

    void AudioSystem::OnStop(Scene& /*scene*/, const SystemContext& context)
    {
        for (const auto& [entity, voice] : m_Voices)
        {
            context.Audio.StopVoice(voice.Voice);
        }
        m_Voices.clear();
        m_Rejected.clear();
        m_SourcePosition.clear();
        m_HasListenerPosition = false;
        // A stopped system no longer speaks for its scene's music.
        context.Audio.SetMusicRequest(std::nullopt);
    }

    void AudioSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        const Audio::ScopedAudio& audio = context.Audio;
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
        // This scene's listener: what its sources here, and every PlayAt its systems fire, are
        // spatialized against, whatever other worlds' listeners are.
        audio.SetListener(listener);

        // The scene's music request, renewed every View update so a runtime edit is live; absent, the
        // scene wants none. A paused scene runs no View pass, so its last request keeps standing.
        if (const MusicState* music = scene.TryGetFirst<MusicState>(); music != nullptr)
        {
            audio.SetMusicRequest(Audio::MusicRequest{.Track = music->Track,
                                                      .FadeSeconds = music->FadeSeconds,
                                                      .Loop = music->Loop,
                                                      .Priority = music->Priority});
        }
        else
        {
            audio.SetMusicRequest(std::nullopt);
        }

        // Drop voices the device retired (surfaced through IsVoiceLive once Pump drained the
        // retired-voice channel). A non-looping clip that played out reads Playing false, so setting
        // it again replays it; anything else the mixer dropped — an evicted looping clip or generator
        // — restarts below while it stays Playing.
        for (auto it = m_Voices.begin(); it != m_Voices.end();)
        {
            if (audio.IsVoiceLive(it->second.Voice))
            {
                ++it;
                continue;
            }
            if (it->second.Shape.Generator == nullptr)
            {
                if (auto* source = scene.TryGet<AudioSource>(it->first);
                    source != nullptr && !source->Looping)
                {
                    source->Playing = false;
                }
            }
            it = m_Voices.erase(it);
        }

        // Gather the sources that should be sounding this update, with their spatialized parameters
        // and post-attenuation loudness (the cap's priority key). The component pointers stay valid
        // through the rest of the update: nothing below adds or removes a component.
        struct Candidate
        {
            Entity Source;
            const AudioSource* Component;
            Audio::VoiceParams Params;
            vec3 Position;
            vec3 Velocity;
        };
        std::vector<Candidate> candidates;
        std::unordered_set<Entity> present;

        scene.Each<Transform, AudioSource>(
            [&](const Entity entity, Transform&, AudioSource& source)
            {
                present.insert(entity);
                if (!source.Playing)
                {
                    return;
                }

                const bool generator = source.Generator != nullptr;
                if (generator && source.Spatial && (source.Channels == 2 || source.Buffered))
                {
                    // The engine places a spatial voice as one mono point with per-frame pan and
                    // Doppler, which a stereo image or an ahead-of-time ring cannot carry.
                    if (m_Rejected.insert(entity).second)
                    {
                        Log::Warn("AudioSystem: entity {} asks for a spatial {} generator voice, "
                                  "which cannot be spatialized; it plays nothing",
                                  entity.Index, source.Buffered ? "buffered" : "stereo");
                    }
                    return;
                }

                // Only starting a clip voice needs the clip: a Pcm clip plays off its resident
                // buffer, an Encoded clip through the streaming path, an unresident clip not at all.
                // A sounding voice holds its own reference to what it plays.
                if (!generator && !m_Voices.contains(entity))
                {
                    const Audio::AudioClip* clip = source.Clip.Get();
                    if (clip == nullptr || (clip->Buffer() == nullptr &&
                                            clip->Storage() != Audio::AudioStorage::Encoded))
                    {
                        return;
                    }
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

                Audio::VoiceParams params = Spatialize(source, position, velocity, listener);
                // A generator never ends; Looping is a clip's.
                params.Loop = params.Loop && !generator;
                candidates.push_back(Candidate{
                    .Source = entity,
                    .Component = &source,
                    .Params = params,
                    .Position = position,
                    .Velocity = velocity,
                });
            });

        // Stop the voice of every source no longer sounding as it was started: gone, stopped, or
        // re-shaped (a replaced generator, a changed generator registration). A re-shaped source
        // starts again below.
        std::unordered_map<Entity, const AudioSource*> wanted;
        wanted.reserve(candidates.size());
        for (const Candidate& candidate : candidates)
        {
            wanted.emplace(candidate.Source, candidate.Component);
        }
        for (auto it = m_Voices.begin(); it != m_Voices.end();)
        {
            const auto want = wanted.find(it->first);
            if (want != wanted.end() && ShapeOf(*want->second) == it->second.Shape)
            {
                ++it;
                continue;
            }
            audio.StopVoice(it->second.Voice);
            it = m_Voices.erase(it);
        }
        std::erase_if(m_SourcePosition,
                      [&](const auto& entry) { return !present.contains(entry.first); });
        std::erase_if(m_Rejected, [&](const Entity entity) { return !present.contains(entity); });

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
                    audio.StopVoice(it->second.Voice);
                    m_Voices.erase(it);
                }
            }
            candidates.resize(m_VoiceCap);
        }

        // Start the new voices and retune the live ones.
        for (const Candidate& candidate : candidates)
        {
            const AudioSource& source = *candidate.Component;
            if (const auto it = m_Voices.find(candidate.Source); it != m_Voices.end())
            {
                const SourceVoice& voice = it->second;
                if (voice.Shape.Generator != nullptr && voice.Shape.Spatial)
                {
                    // A positioned generator voice is spatialized by the engine from its mix and
                    // pose, against the same listener this update set.
                    audio.SetVoiceMix(voice.Voice, MixOf(source));
                    audio.SetVoicePose(voice.Voice, candidate.Position, candidate.Velocity);
                }
                else
                {
                    audio.SetVoiceParams(voice.Voice, candidate.Params);
                }
                continue;
            }

            const VoiceShape shape = ShapeOf(source);
            Audio::VoiceHandle handle;
            if (source.Generator != nullptr)
            {
                // The instance's previous voice is still being released; registering it again now
                // would have two voices render it.
                if (audio.IsGeneratorInUse(*source.Generator))
                {
                    continue;
                }
                Audio::GeneratorVoiceParams params{.Bus = candidate.Params.Bus,
                                                   .Spatial = shape.Spatial,
                                                   .Channels = shape.Channels,
                                                   .Buffered = shape.Buffered,
                                                   .BufferSeconds = source.BufferSeconds,
                                                   .Gain = candidate.Params.Gain,
                                                   .Pitch = candidate.Params.Pitch};
                if (shape.Spatial)
                {
                    const Audio::SpatialVoiceMix mix = MixOf(source);
                    params.Gain = mix.Gain;
                    params.Pitch = mix.Pitch;
                    params.Position = candidate.Position;
                    params.Velocity = candidate.Velocity;
                    params.MinDistance = mix.MinDistance;
                    params.MaxDistance = mix.MaxDistance;
                    params.OcclusionFactor = mix.OcclusionFactor;
                }
                handle = audio.PlayGenerator(source.Generator, params);
            }
            else
            {
                handle = audio.AddClipVoice(source.Clip, candidate.Params);
            }
            if (handle.IsValid())
            {
                m_Voices[candidate.Source] = SourceVoice{.Voice = handle, .Shape = shape};
            }
        }
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
