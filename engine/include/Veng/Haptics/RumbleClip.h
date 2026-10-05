#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Math/Curve.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng::Haptics
{
    /// @brief The reflected payload of a rumble clip: a keyframed animation of a pad's motors.
    ///
    /// The single record the cook writes and the loader reads through the shared WriteFields /
    /// ReadFields encoder, and the document a `*.rumble.json` source binds through the JSON walker,
    /// keyed by these field names. Each channel is a Curve1D over [0, Duration] with values in
    /// [0, 1]; an empty channel is silent. A new field evolves tolerantly within the fixed
    /// CookedRumbleClipVersion.
    struct RumbleClipData
    {
        /// @brief The clip's length in seconds; greater than zero.
        f32 Duration = 0.0f;
        /// @brief Whether the clip wraps at Duration rather than ending; a play may override it.
        bool Loop = false;
        /// @brief The low-frequency (heavy) grip motor.
        Curve1D LowFrequency;
        /// @brief The high-frequency (light) grip motor.
        Curve1D HighFrequency;
        /// @brief The left trigger's motor; ignored on a pad without trigger motors.
        Curve1D LeftTrigger;
        /// @brief The right trigger's motor; ignored on a pad without trigger motors.
        Curve1D RightTrigger;
    };

    /// @brief Why a rumble clip is not playable, or empty when it is.
    ///
    /// The one statement of what makes a clip valid, shared by the cook and anything else that builds
    /// one: a finite Duration greater than zero, at least one channel with a key, and in every channel
    /// keys sorted by time, each time within [0, Duration] and each value within [0, 1]. The message
    /// names the channel and the index of the offending key.
    /// @param clip  The clip to check.
    /// @return The first violation found, or an empty string.
    [[nodiscard]] string CheckRumbleClip(const RumbleClipData& clip);

    /// @brief A cooked rumble clip: a CPU-only asset holding its motor curves.
    ///
    /// Cooked from a `*.rumble.json` source and loaded by AssetId through the ordinary path; played
    /// through HapticsEngine::Play. The data is immutable once built, so a clip is shared freely
    /// between every instance playing it.
    class RumbleClip
    {
    public:
        /// @brief Creates a clip from its decoded data.
        /// @param data  The clip's duration, loop flag and channels.
        /// @return The constructed clip.
        static Ref<RumbleClip> Create(RumbleClipData data);

        /// @brief Returns the clip's duration, loop flag and channels.
        [[nodiscard]] const RumbleClipData& GetData() const { return m_Data; }

        /// @brief Returns the clip's length in seconds.
        [[nodiscard]] f32 GetDuration() const { return m_Data.Duration; }

        /// @brief Whether the clip loops unless a play overrides it.
        [[nodiscard]] bool IsLooping() const { return m_Data.Loop; }

    private:
        explicit RumbleClip(RumbleClipData data);

        /// @brief The clip's duration, loop flag and channels.
        RumbleClipData m_Data;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping Haptics::RumbleClip to AssetTypes::RumbleClip.
    template <>
    struct AssetTypeTrait<Haptics::RumbleClip>
    {
        /// @brief The asset type tag for RumbleClip.
        static constexpr AssetTypeId Type = AssetTypes::RumbleClip;
    };
}

VE_REFLECT(::Veng::Haptics::RumbleClipData, 0x6EE5BDED80A26ED5ULL)
VE_FIELD(Duration, .DisplayName = "Duration", .Tooltip = "The clip's length in seconds.")
VE_FIELD(Loop, .DisplayName = "Loop", .Tooltip = "Whether the clip wraps at its duration.")
VE_FIELD(LowFrequency, .DisplayName = "Low Frequency", .Tooltip = "The heavy grip motor, 0..1.")
VE_FIELD(HighFrequency, .DisplayName = "High Frequency", .Tooltip = "The light grip motor, 0..1.")
VE_FIELD(LeftTrigger, .DisplayName = "Left Trigger", .Tooltip = "The left trigger's motor, 0..1.")
VE_FIELD(RightTrigger, .DisplayName = "Right Trigger",
         .Tooltip = "The right trigger's motor, 0..1.")
VE_REFLECT_END();
