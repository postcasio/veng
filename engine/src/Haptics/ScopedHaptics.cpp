#include <Veng/Haptics/ScopedHaptics.h>

#include <Veng/Haptics/Haptics.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Haptics
{
    GamepadId ResolveRumbleTarget(const RumbleTarget& target, const Scene* scene,
                                  const Input& input)
    {
        switch (target.Kind)
        {
        case RumbleTargetKind::Seat:
        {
            if (target.Seat.IsNull())
            {
                const std::span<const GamepadId> connected = input.ConnectedGamepads();
                return connected.empty() ? GamepadId::None : connected.front();
            }
            if (scene == nullptr || !scene->IsAlive(target.Seat))
            {
                return GamepadId::None;
            }
            const auto* seat = scene->TryGet<SeatInput>(target.Seat);
            return seat != nullptr ? seat->Gamepad : GamepadId::None;
        }
        case RumbleTargetKind::Gamepad:
            return target.Gamepad;
        case RumbleTargetKind::None:
            break;
        }
        return GamepadId::None;
    }

    ScopedHaptics::ScopedHaptics(HapticsEngine& engine, const PresentationScopeId scope,
                                 const Input& input, const Scene* scene, const bool isReplay)
        : m_Engine(&engine), m_Scope(scope), m_Input(&input), m_Scene(scene), m_IsReplay(isReplay)
    {
    }

    void ScopedHaptics::PlayOneShot(const RumbleTarget& target, const AssetHandle<RumbleClip>& clip,
                                    const f32 intensity) const
    {
        if (m_IsReplay || m_Engine == nullptr)
        {
            return;
        }
        m_Engine->PlayOneShot(m_Scope, Resolve(target), clip, intensity);
    }

    void ScopedHaptics::Submit(const GamepadId pad, const RumbleChannels& channels) const
    {
        if (m_Engine != nullptr)
        {
            m_Engine->SubmitLayer(m_Scope, pad, channels);
        }
    }

    GamepadId ScopedHaptics::Resolve(const RumbleTarget& target) const
    {
        return m_Input != nullptr ? ResolveRumbleTarget(target, m_Scene, *m_Input)
                                  : GamepadId::None;
    }
}
