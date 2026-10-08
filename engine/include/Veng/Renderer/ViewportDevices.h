#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Input;
}

namespace Veng::Audio
{
    class AudioEngine;
}

namespace Veng::Haptics
{
    class HapticsEngine;
}

namespace Veng::Renderer
{
    /// @brief The device engines a viewport hands the Gui drivers it drives.
    ///
    /// Held by a ViewportCompositor (the Application sets it once) and handed to every viewport it
    /// registers, so a driver in any registered viewport — managed, consumer-registered, an editor
    /// document's — reaches sound and rumble. A viewport builds each driver's facades from these and
    /// the presentation scope of the scene the driver drives, so what a driver plays belongs to that
    /// scene. Every engine is borrowed and outlives the compositor's drives; a null one hands the
    /// driver an unbound facade, which plays nothing.
    struct ViewportDevices
    {
        /// @brief The audio engine a driver's sound starts in, or null for none.
        Audio::AudioEngine* Audio = nullptr;
        /// @brief The haptics engine a driver's rumble plays on, or null for none.
        Haptics::HapticsEngine* Haptics = nullptr;
        /// @brief The input the implicit seat's rumble target resolves against, or null for none.
        const Veng::Input* Input = nullptr;
    };
}
