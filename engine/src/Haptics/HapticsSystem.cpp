#include <Veng/Haptics/HapticsSystem.h>

#include <Veng/Haptics/Haptics.h>
#include <Veng/Haptics/RumbleSource.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        bool IsLooping(const RumbleSource& source, const Haptics::RumbleClip& clip)
        {
            switch (source.Loop)
            {
            case Haptics::RumbleLoop::Always:
                return true;
            case Haptics::RumbleLoop::Once:
                return false;
            case Haptics::RumbleLoop::FromClip:
                break;
            }
            return clip.IsLooping();
        }
    }

    void HapticsSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        const f32 step = std::max(0.0f, delta);
        scene.Each<RumbleSource>(
            [&](const Entity, RumbleSource& source)
            {
                // A rise restarts the clip from its start, which sounds this frame.
                const bool rose = source.Playing && !source.WasPlaying;
                source.WasPlaying = source.Playing;
                if (rose)
                {
                    source.Time = 0.0f;
                    source.Fade = 1.0f;
                }
                // A clip still loading holds its time, so it starts from where it was once resident.
                if (!(source.Fade > 0.0f) || !source.Clip.IsLoaded())
                {
                    return;
                }
                if (!rose)
                {
                    source.Time += step;
                }
                if (!source.Playing)
                {
                    source.Fade = source.FadeOutSeconds > 0.0f
                                      ? std::max(0.0f, source.Fade - step / source.FadeOutSeconds)
                                      : 0.0f;
                }

                const Haptics::RumbleClip& clip = *source.Clip.Get();
                const bool loop = IsLooping(source, clip);
                if (!loop && source.Time >= clip.GetDuration())
                {
                    source.Playing = false;
                    source.WasPlaying = false;
                    source.Fade = 0.0f;
                    return;
                }

                const GamepadId pad = Haptics::ResolveRumbleTarget(
                    Haptics::RumbleTarget{
                        .Kind = source.Target, .Seat = source.Seat, .Gamepad = source.Gamepad},
                    &scene, context.Input);
                const Haptics::RumbleChannels level =
                    Haptics::EvaluateClip(clip.GetData(), source.Time, loop);
                context.Haptics.Submit(
                    pad,
                    Haptics::ScaleRumble(level, std::max(0.0f, source.Intensity) * source.Fade));
            });
    }
}
