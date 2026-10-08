#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Scene/PresentationScope.h>

namespace Veng::TestSupport
{
    /// @brief Returns a scope registry that lives for the whole process, for audio devices a case
    ///        builds outside a TestServices bundle.
    ///
    /// Deliberately never destroyed, so no static-destruction order can close it under a device. A
    /// case that opens scopes on it closes them itself (dropping the handles), and a case that judges
    /// scope states calls Resolve on a registry of its own rather than this shared one.
    inline PresentationScopes& SharedPresentationScopes()
    {
        static auto* const scopes = new PresentationScopes();
        return *scopes;
    }

    /// @brief Returns the always-Live application scope of SharedPresentationScopes().
    inline PresentationScopeId AppScope()
    {
        return SharedPresentationScopes().GetApplicationScope();
    }

    /// @brief Creates a null audio device judged by SharedPresentationScopes().
    /// @param sampleRate  The output rate in Hz.
    /// @param channels    The output channel count.
    inline Unique<Audio::AudioDevice> MakeNullAudioDevice(const u32 sampleRate = 48000,
                                                          const u32 channels = 2)
    {
        return Audio::AudioDevice::Create(
            SharedPresentationScopes(), Audio::AudioDeviceInfo{.Backend = Audio::AudioBackend::Null,
                                                               .SampleRate = sampleRate,
                                                               .Channels = channels});
    }
}
