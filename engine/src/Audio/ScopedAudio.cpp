#include <Veng/Audio/ScopedAudio.h>

#include <Veng/Assert.h>

namespace Veng::Audio
{
    ScopedAudio::ScopedAudio(AudioEngine& engine, const PresentationScopeId scope,
                             const bool isReplay)
        : m_Engine(&engine), m_Scope(scope), m_IsReplay(isReplay)
    {
    }

    VoiceHandle ScopedAudio::PlayOneShot(const AssetHandle<AudioClip>& clip,
                                         const OneShotParams& params) const
    {
        return CanStart() ? m_Engine->PlayOneShot(m_Scope, clip, params) : VoiceHandle{};
    }

    VoiceHandle ScopedAudio::PlayAt(const AssetHandle<AudioClip>& clip, const vec3 worldPos,
                                    const SpatialOneShotParams& params) const
    {
        return CanStart() ? m_Engine->PlayAt(m_Scope, clip, worldPos, params) : VoiceHandle{};
    }

    VoiceHandle ScopedAudio::PlayGenerator(Ref<IAudioGenerator> generator,
                                           const GeneratorVoiceParams& params) const
    {
        return CanStart() ? m_Engine->PlayGenerator(m_Scope, std::move(generator), params)
                          : VoiceHandle{};
    }

    VoiceHandle ScopedAudio::AddClipVoice(const AssetHandle<AudioClip>& clip,
                                          const VoiceParams& params) const
    {
        return CanStart() ? m_Engine->AddClipVoice(m_Scope, clip, params) : VoiceHandle{};
    }

    void ScopedAudio::SetVoicePose(const VoiceHandle voice, const vec3 worldPos,
                                   const vec3 velocity) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SetVoicePose(voice, worldPos, velocity);
        }
    }

    void ScopedAudio::SetVoiceMix(const VoiceHandle voice, const SpatialVoiceMix& mix) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SetVoiceMix(voice, mix);
        }
    }

    bool ScopedAudio::IsGeneratorInUse(const IAudioGenerator& generator) const
    {
        return m_Engine != nullptr && m_Engine->IsGeneratorInUse(generator);
    }

    void ScopedAudio::SetVoiceParams(const VoiceHandle voice, const VoiceParams& params) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SetVoiceParams(voice, params);
        }
    }

    void ScopedAudio::StopVoice(const VoiceHandle voice) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->StopVoice(voice);
        }
    }

    bool ScopedAudio::IsVoiceLive(const VoiceHandle voice) const
    {
        return m_Engine != nullptr && m_Engine->IsVoiceLive(voice);
    }

    optional<VoiceParams> ScopedAudio::GetVoiceParams(const VoiceHandle voice) const
    {
        return m_Engine != nullptr ? m_Engine->GetVoiceParams(voice) : std::nullopt;
    }

    void ScopedAudio::SetListener(const ListenerPose& listener) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SetListener(m_Scope, listener);
        }
    }

    AssetHandle<AudioClip> ScopedAudio::CreateClip(const std::span<const f32> samples,
                                                   const AudioBufferFormat format) const
    {
        return m_Engine != nullptr ? m_Engine->CreateClip(samples, format)
                                   : AssetHandle<AudioClip>{};
    }

    u32 ScopedAudio::GetOutputSampleRate() const
    {
        return m_Engine != nullptr ? m_Engine->GetOutputSampleRate() : 0;
    }

    void ScopedAudio::SetMusicRequest(const optional<MusicRequest>& request) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SetMusicRequest(m_Scope, request);
        }
    }
}
