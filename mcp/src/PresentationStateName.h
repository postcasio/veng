#pragma once

#include <Veng/Scene/PresentationScope.h>

namespace Veng::Mcp
{
    /// @brief A presentation state's name as the tools report it: live, muted, held or closed.
    /// @param state  The state.
    /// @return Its lowercase name.
    inline const char* PresentationStateName(const PresentationState state)
    {
        switch (state)
        {
        case PresentationState::Live:
            return "live";
        case PresentationState::Muted:
            return "muted";
        case PresentationState::Held:
            return "held";
        case PresentationState::Closed:
            break;
        }
        return "closed";
    }
}
