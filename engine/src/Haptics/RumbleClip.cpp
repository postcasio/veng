#include <Veng/Haptics/RumbleClip.h>

#include <cmath>

#include <fmt/format.h>

namespace Veng::Haptics
{
    namespace
    {
        // Why one channel breaks the clip's rules, or empty when it does not.
        string CheckChannel(const string_view name, const Curve1D& curve, const f32 duration)
        {
            for (usize index = 0; index < curve.Keys.size(); ++index)
            {
                const CurveKey& key = curve.Keys[index];
                if (!std::isfinite(key.Time) || key.Time < 0.0f || key.Time > duration)
                {
                    return fmt::format("channel '{}' key {} has Time {}, outside the clip's "
                                       "[0, {}]",
                                       name, index, key.Time, duration);
                }
                if (!std::isfinite(key.Value) || key.Value < 0.0f || key.Value > 1.0f)
                {
                    return fmt::format("channel '{}' key {} has Value {}, outside [0, 1]", name,
                                       index, key.Value);
                }
                if (index > 0 && key.Time < curve.Keys[index - 1].Time)
                {
                    return fmt::format("channel '{}' key {} at Time {} comes before key {} at "
                                       "Time {}; keys must be sorted by time",
                                       name, index, key.Time, index - 1,
                                       curve.Keys[index - 1].Time);
                }
            }
            return {};
        }
    }

    string CheckRumbleClip(const RumbleClipData& clip)
    {
        if (!std::isfinite(clip.Duration) || clip.Duration <= 0.0f)
        {
            return fmt::format("Duration {} must be greater than zero", clip.Duration);
        }
        if (clip.LowFrequency.IsEmpty() && clip.HighFrequency.IsEmpty() &&
            clip.LeftTrigger.IsEmpty() && clip.RightTrigger.IsEmpty())
        {
            return "every channel is empty; a clip needs at least one key";
        }

        const std::pair<string_view, const Curve1D*> channels[] = {
            {"LowFrequency", &clip.LowFrequency},
            {"HighFrequency", &clip.HighFrequency},
            {"LeftTrigger", &clip.LeftTrigger},
            {"RightTrigger", &clip.RightTrigger},
        };
        for (const auto& [name, curve] : channels)
        {
            if (string bad = CheckChannel(name, *curve, clip.Duration); !bad.empty())
            {
                return bad;
            }
        }
        return {};
    }

    RumbleClip::RumbleClip(RumbleClipData data) : m_Data(std::move(data)) {}

    Ref<RumbleClip> RumbleClip::Create(RumbleClipData data)
    {
        return Ref<RumbleClip>(new RumbleClip(std::move(data)));
    }
}
