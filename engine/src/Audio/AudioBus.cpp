#include <Veng/Audio/AudioBus.h>

namespace Veng::Audio
{
    const char* ToString(AudioBus bus)
    {
        switch (bus)
        {
        case AudioBus::Master:
            return "Master";
        case AudioBus::Music:
            return "Music";
        case AudioBus::SFX:
            return "SFX";
        case AudioBus::UI:
            return "UI";
        case AudioBus::Ambience:
            return "Ambience";
        }
        return "Unknown";
    }
}
