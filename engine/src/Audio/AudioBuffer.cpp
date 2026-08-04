#include <Veng/Audio/AudioBuffer.h>

#include <Veng/Assert.h>

namespace Veng::Audio
{
    AudioBuffer::AudioBuffer(std::span<const f32> interleaved, u32 channels, u32 sampleRate)
        : m_Samples(interleaved.begin(), interleaved.end()), m_Channels(channels),
          m_SampleRate(sampleRate)
    {
        VE_ASSERT(channels > 0, "AudioBuffer requires at least one channel");
        VE_ASSERT(interleaved.size() % channels == 0,
                  "AudioBuffer sample count {} is not a multiple of channel count {}",
                  interleaved.size(), channels);
        m_FrameCount = interleaved.size() / channels;
    }

    Ref<AudioBuffer> AudioBuffer::Create(std::span<const f32> interleaved, u32 channels,
                                         u32 sampleRate)
    {
        return Ref<AudioBuffer>(new AudioBuffer(interleaved, channels, sampleRate));
    }
}
