#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Haptics/ScopedHaptics.h>
#include <Veng/Input.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/PresentationScope.h>

#include <span>

namespace Veng::Haptics
{
    /// @brief One pad's four motor levels, each 0..1: what a clip evaluates to and what a pad mixes.
    struct RumbleChannels
    {
        /// @brief The low-frequency (heavy) grip motor.
        f32 LowFrequency = 0.0f;
        /// @brief The high-frequency (light) grip motor.
        f32 HighFrequency = 0.0f;
        /// @brief The left trigger's motor.
        f32 LeftTrigger = 0.0f;
        /// @brief The right trigger's motor.
        f32 RightTrigger = 0.0f;

        /// @brief Compares every channel.
        bool operator==(const RumbleChannels&) const = default;
    };

    /// @brief Evaluates a clip's four channels at a time, each clamped to [0, 1].
    ///
    /// The pure core of playback. A looping clip wraps the time into [0, Duration); a non-looping
    /// clip reads zero from Duration on, and zero before 0. A clip with a non-positive duration reads
    /// zero everywhere.
    /// @param clip  The clip's data.
    /// @param time  The time since the clip started, in seconds.
    /// @param loop  Whether the playback wraps (a RumbleSource's resolved RumbleLoop).
    /// @return The channel levels.
    [[nodiscard]] RumbleChannels EvaluateClip(const RumbleClipData& clip, f32 time, bool loop);

    /// @brief Scales every channel by one factor, unclamped.
    /// @param channels  The levels.
    /// @param scale     The factor (an intensity times a fade).
    /// @return The scaled levels.
    [[nodiscard]] RumbleChannels ScaleRumble(const RumbleChannels& channels, f32 scale);

    /// @brief Mixes layered rumble: per channel, the maximum over the layers, scaled by a master
    ///        intensity and clamped to [0, 1].
    ///
    /// Maximum rather than sum, so overlapping effects never saturate: a strong pulse rises above a
    /// hum and the hum returns after it, with no priority between them.
    /// @param layers  Each sounding one-shot's and submitted layer's scaled levels.
    /// @param master  The master intensity applied after the maximum.
    /// @return The mixed levels; zero for no layers.
    [[nodiscard]] RumbleChannels MixRumble(std::span<const RumbleChannels> layers,
                                           f32 master = 1.0f);

    /// @brief What a rumble plays on.
    enum class RumbleTargetKind : u8
    {
        /// @brief Nothing: a rumble targeting it is silent.
        None,
        /// @brief A seat of the calling scene, resolved to the pad its SeatInput names.
        Seat,
        /// @brief One pad slot directly, for an application without seats.
        Gamepad,
    };

    /// @brief How a RumbleSource's clip runs out.
    enum class RumbleLoop : u8
    {
        /// @brief As the clip itself says (RumbleClipData::Loop).
        FromClip,
        /// @brief Wraps at the clip's duration whatever the clip says.
        Always,
        /// @brief Plays the clip through once whatever the clip says.
        Once,
    };

    /// @brief Where a rumble plays: a seat of the calling scene, or a raw pad slot.
    ///
    /// Scene-local: a seat is an entity in the scene whose system names it, resolved against that
    /// scene (ResolveRumbleTarget), so two scenes' seats that share an entity handle never alias.
    struct RumbleTarget
    {
        /// @brief Which of the two the target names.
        RumbleTargetKind Kind = RumbleTargetKind::None;
        /// @brief The seat's Viewer entity, for a Seat target; Entity::Null is the implicit seat.
        ///
        /// Resolved through the entity's SeatInput::Gamepad, so a seat with no pad is silent. The
        /// implicit seat reads every device, so it resolves to the first connected pad.
        Entity Seat = Entity::Null;
        /// @brief The pad slot, for a Gamepad target.
        GamepadId Gamepad = GamepadId::None;

        /// @brief Returns a target naming a seat of the calling scene.
        /// @param seat  The seat's Viewer entity, or Entity::Null for the implicit seat.
        [[nodiscard]] static RumbleTarget ForSeat(const Entity seat)
        {
            return RumbleTarget{.Kind = RumbleTargetKind::Seat, .Seat = seat};
        }

        /// @brief Returns a target naming one pad slot.
        /// @param pad  The slot.
        [[nodiscard]] static RumbleTarget ForGamepad(const GamepadId pad)
        {
            return RumbleTarget{.Kind = RumbleTargetKind::Gamepad, .Gamepad = pad};
        }

        /// @brief Compares the kind and the member that kind uses.
        bool operator==(const RumbleTarget& other) const
        {
            if (Kind != other.Kind)
            {
                return false;
            }
            switch (Kind)
            {
            case RumbleTargetKind::Seat:
                return Seat == other.Seat;
            case RumbleTargetKind::Gamepad:
                return Gamepad == other.Gamepad;
            case RumbleTargetKind::None:
                break;
            }
            return true;
        }
    };

    /// @brief Resolves a target to the pad it plays on now, in the scene it names a seat of.
    ///
    /// A seat entity resolves to its SeatInput::Gamepad (None when the entity, its SeatInput or a
    /// scene to read it in is missing); the implicit seat to the first pad @p input reports
    /// connected; a pad target to itself.
    /// @param target  The target.
    /// @param scene   The scene a seat target names an entity of; null resolves every seat entity to
    ///                no pad.
    /// @param input   The input whose connected pads the implicit seat reads.
    /// @return The pad slot, or GamepadId::None.
    [[nodiscard]] GamepadId ResolveRumbleTarget(const RumbleTarget& target, const Scene* scene,
                                                const Input& input);

    /// @brief A read-only view of one one-shot the engine holds, for tooling and tests.
    struct RumbleOneShotInfo
    {
        /// @brief The presentation scope that owns it.
        PresentationScopeId Scope;
        /// @brief That scope's state now (Live, Muted, Held, or Closed until the next update).
        PresentationState State = PresentationState::Closed;
        /// @brief The pad it plays on, resolved when it was played.
        GamepadId Gamepad = GamepadId::None;
        /// @brief The clip playing.
        AssetId Clip;
        /// @brief Seconds it has advanced since it started.
        f32 Time = 0.0f;
        /// @brief The clip's duration in seconds.
        f32 Duration = 0.0f;
        /// @brief The intensity it plays at.
        f32 Intensity = 1.0f;
    };

    /// @brief A read-only view of one layer the last update mixed, for tooling and tests.
    struct RumbleLayerInfo
    {
        /// @brief The presentation scope that submitted it.
        PresentationScopeId Scope;
        /// @brief That scope's state now.
        PresentationState State = PresentationState::Closed;
        /// @brief The pad it was submitted to.
        GamepadId Gamepad = GamepadId::None;
        /// @brief Its levels, before the master intensity.
        RumbleChannels Channels;
    };

    /// @brief Where the engine's mixed output goes.
    struct HapticsEngineInfo
    {
        /// @brief Receives each pad slot's levels once per Update; unset drives no device.
        ///
        /// Called for every slot, 0 through Input::MaxGamepads - 1, with zeros where nothing plays
        /// or the output is suspended.
        function<void(GamepadId pad, const RumbleChannels& levels)> WriteMotors;
    };

    /// @brief One frame's input to HapticsEngine::Update.
    struct HapticsFrameInfo
    {
        /// @brief Seconds since the last Update, unscaled by any world's time.
        f32 Delta = 0.0f;
        /// @brief Whether the device output is silenced this frame (the window lost focus).
        ///
        /// One-shots keep advancing, so a pulse that ended meanwhile does not replay; only what
        /// reaches WriteMotors is zeroed.
        bool OutputSuspended = false;
    };

    /// @brief The per-pad rumble mixer: the application's one writer of every pad's motors.
    ///
    /// Holds two things, each owned by a presentation scope: the **one-shots** played through a
    /// ScopedHaptics (a clip, the pad it resolved to, its time and intensity), and **this frame's
    /// layers** submitted through one (a RumbleSource's evaluated level, as HapticsSystem submits it).
    /// Once per frame, after PresentationScopes::Resolve, Update judges each one-shot by its scope's
    /// state — Closed drops it, Held freezes it, Muted and Live advance it, and a finished one is
    /// dropped — then mixes every pad over the one-shots and layers whose scope is Live (MixRumble,
    /// scaled by the master intensity), hands each slot's levels to the host, and clears the layers.
    /// It runs the same headless and on a dedicated host, where the host has no device to write.
    ///
    /// Systems reach it only through their context's scoped facade (SystemContext::Haptics);
    /// application code and tooling through Application::GetApplicationHaptics. Main-thread only,
    /// like the rest of the frame.
    class HapticsEngine
    {
    public:
        /// @brief Constructs an engine over the scope registry it judges its output by.
        /// @param scopes  The registry every scope it is handed belongs to; must outlive the engine.
        /// @param info    Where its output goes.
        explicit HapticsEngine(const PresentationScopes& scopes, HapticsEngineInfo info = {});

        /// @brief Destroys the engine, releasing every clip it holds.
        ~HapticsEngine();

        /// @brief Not copyable: it is the one writer of the motors.
        HapticsEngine(const HapticsEngine&) = delete;

        /// @brief Not copyable: it is the one writer of the motors.
        HapticsEngine& operator=(const HapticsEngine&) = delete;

        /// @brief Starts a clip once through on a pad, owned by a scope.
        ///
        /// Fire and forget: it plays the clip through once (whatever the clip's own Loop) and is
        /// dropped when it ends or its scope closes. It first sounds its time-zero level at the next
        /// Update its scope is not Held. A clip that is not loaded starts nothing and logs once per
        /// clip; so does a pad of None (without the log), and a scope already Closed.
        /// @param scope      The scope that owns it.
        /// @param pad        The pad slot it plays on.
        /// @param clip       The clip; held resident while it plays.
        /// @param intensity  Scales every channel; clamped to 0 or more.
        void PlayOneShot(PresentationScopeId scope, GamepadId pad,
                         const AssetHandle<RumbleClip>& clip, f32 intensity = 1.0f);

        /// @brief Adds a layer to a pad's mix for the coming Update only.
        ///
        /// A continuous effect submits its current level every frame; one that stops submitting is
        /// silent from the next Update. Ignored for a pad of None or out of range.
        /// @param scope     The scope that owns it.
        /// @param pad       The pad slot.
        /// @param channels  The levels, already scaled by the caller's intensity.
        void SubmitLayer(PresentationScopeId scope, GamepadId pad, const RumbleChannels& channels);

        /// @brief Drops at once every one-shot a scope owns on a pad, for tooling.
        /// @param scope  The owning scope.
        /// @param pad    The pad slot.
        void StopOneShots(PresentationScopeId scope, GamepadId pad);

        /// @brief Sets the master intensity every pad's mix is scaled by, clamped to [0, 1].
        ///
        /// The hook for a player's vibration setting; 1 by default.
        /// @param intensity  The master intensity.
        void SetMasterIntensity(f32 intensity);

        /// @brief Returns the master intensity.
        [[nodiscard]] f32 GetMasterIntensity() const;

        /// @brief Returns every one-shot held, in the order they were played.
        [[nodiscard]] vector<RumbleOneShotInfo> GetOneShots() const;

        /// @brief Returns the layers the last Update mixed, in the order they were submitted.
        [[nodiscard]] vector<RumbleLayerInfo> GetLayers() const;

        /// @brief Returns a pad's mixed levels as of the last Update, before any output suspension.
        /// @param pad  The pad slot; an out-of-range slot reads zero.
        [[nodiscard]] RumbleChannels GetOutput(GamepadId pad) const;

        /// @brief Whether the last Update silenced the device output.
        [[nodiscard]] bool IsOutputSuspended() const;

        /// @brief Returns the scope registry the engine judges its output by.
        [[nodiscard]] const PresentationScopes& GetScopes() const;

        /// @brief Advances the one-shots, mixes every pad, writes it and clears the layers.
        ///
        /// Called once per frame, after PresentationScopes::Resolve has latched this frame's states.
        /// @param frame  The frame's delta and output suspension.
        void Update(const HapticsFrameInfo& frame);

    private:
        /// @brief The one-shot table, the layers, the per-pad mix and the logged-clip set.
        struct State;

        /// @brief The one-shot table, the layers, the per-pad mix and the logged-clip set.
        Unique<State> m_State;
    };
}

VE_ENUM(::Veng::Haptics::RumbleTargetKind, 0x60B26D670CF337E3ULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(Seat)
VE_ENUMERATOR(Gamepad)
VE_ENUM_END();

VE_ENUM(::Veng::Haptics::RumbleLoop, 0xDC0110FDE061E226ULL)
VE_ENUMERATOR(FromClip)
VE_ENUMERATOR(Always)
VE_ENUMERATOR(Once)
VE_ENUM_END();
