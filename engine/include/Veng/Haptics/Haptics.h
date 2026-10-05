#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Input.h>
#include <Veng/Input/SeatRef.h>
#include <Veng/WorldInstanceId.h>

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
    /// @param loop  The instance's effective loop setting (the clip's own unless a play overrode it).
    /// @return The channel levels.
    [[nodiscard]] RumbleChannels EvaluateClip(const RumbleClipData& clip, f32 time, bool loop);

    /// @brief Scales every channel by one factor, unclamped.
    /// @param channels  The levels.
    /// @param scale     The factor (an instance's intensity times its stop fade).
    /// @return The scaled levels.
    [[nodiscard]] RumbleChannels ScaleRumble(const RumbleChannels& channels, f32 scale);

    /// @brief Mixes layered rumble: per channel, the maximum over the layers, scaled by a master
    ///        intensity and clamped to [0, 1].
    ///
    /// Maximum rather than sum, so overlapping effects never saturate: a strong pulse rises above a
    /// hum and the hum returns after it, with no priority between them.
    /// @param layers  Each playing instance's scaled levels.
    /// @param master  The master intensity applied after the maximum.
    /// @return The mixed levels; zero for no layers.
    [[nodiscard]] RumbleChannels MixRumble(std::span<const RumbleChannels> layers,
                                           f32 master = 1.0f);

    /// @brief What a rumble instance plays on.
    enum class RumbleTargetKind : u8
    {
        /// @brief Nothing: an instance targeting it is silent.
        None,
        /// @brief A seat, resolved to its assigned pad every frame.
        Seat,
        /// @brief One pad slot directly, for an application without seats.
        Gamepad,
    };

    /// @brief Where a clip plays: a seat (following its pad assignment) or a raw pad slot.
    struct RumbleTarget
    {
        /// @brief Which of the two the target names.
        RumbleTargetKind Kind = RumbleTargetKind::None;
        /// @brief The seat, for a Seat target.
        ///
        /// Resolved through SeatInput::Gamepad on the seat's Viewer every frame, so the rumble
        /// follows a reassignment and is silent while the seat has no pad. The implicit seat (a null
        /// Viewer) reads every device, so it resolves to the first connected pad.
        SeatRef Seat;
        /// @brief The pad slot, for a Gamepad target.
        GamepadId Gamepad = GamepadId::None;

        /// @brief Returns a target naming a seat.
        /// @param seat  The seat.
        [[nodiscard]] static RumbleTarget ForSeat(const SeatRef& seat)
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

    /// @brief How one play of a clip runs.
    struct RumbleParams
    {
        /// @brief Scales every channel of the clip; changed later through SetIntensity.
        f32 Intensity = 1.0f;
        /// @brief Overrides the clip's own Loop when set.
        optional<bool> Loop;
        /// @brief The world the instance belongs to; invalid means application-owned.
        ///
        /// An instance with a world holds its position and plays nothing while that world is
        /// paused, and stops when the world closes. A scene system passes SystemContext::World.
        WorldInstanceId World;
    };

    /// @brief Names one playing instance; slot plus generation, so a stale handle names nothing.
    struct RumbleHandle
    {
        /// @brief The instance's slot.
        u32 Slot = 0;
        /// @brief The slot's generation when the instance started; zero is the null handle.
        u32 Generation = 0;

        /// @brief Whether the handle was returned by a play that started something.
        [[nodiscard]] bool IsValid() const { return Generation != 0; }

        /// @brief Compares slot and generation.
        bool operator==(const RumbleHandle&) const = default;
    };

    /// @brief A read-only view of one live instance, for tooling and tests.
    struct RumbleInstanceInfo
    {
        /// @brief The instance's handle.
        RumbleHandle Handle;
        /// @brief What the instance plays on.
        RumbleTarget Target;
        /// @brief The pad the target resolved to at the last Update, or GamepadId::None.
        GamepadId Gamepad = GamepadId::None;
        /// @brief The clip playing.
        AssetId Clip;
        /// @brief Seconds since the instance started, unwrapped.
        f32 Time = 0.0f;
        /// @brief The clip's duration in seconds.
        f32 Duration = 0.0f;
        /// @brief The instance's intensity.
        f32 Intensity = 1.0f;
        /// @brief The stop fade's remaining fraction: 1 unless Stop with a fade is ramping it out.
        f32 Fade = 1.0f;
        /// @brief The instance's effective loop setting.
        bool Loop = false;
        /// @brief Whether a fading Stop is in progress.
        bool Stopping = false;
        /// @brief Whether the instance's world is paused, so it holds and plays nothing.
        bool Paused = false;
        /// @brief The world the instance belongs to; invalid for an application-owned instance.
        WorldInstanceId World;
    };

    /// @brief What the engine is told about a world an instance belongs to.
    enum class HapticsWorldState : u8
    {
        /// @brief The world is open and running.
        Open,
        /// @brief The world is open and paused.
        Paused,
        /// @brief The world no longer resolves.
        Closed,
    };

    /// @brief What the engine needs from its host: how to resolve seats and worlds, and where the
    ///        mixed output goes.
    ///
    /// Every member is optional, so a test builds an engine over exactly the hooks it exercises.
    struct HapticsEngineInfo
    {
        /// @brief Resolves a seat to the pad it is assigned; unset leaves every seat padless.
        function<GamepadId(const SeatRef& seat)> ResolveSeat;
        /// @brief Reports a world's state; unset treats every world as open.
        function<HapticsWorldState(WorldInstanceId world)> WorldState;
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
        /// Instances keep advancing, so a hum returns when it lifts and a pulse that ended meanwhile
        /// does not replay; only what reaches WriteMotors is zeroed.
        bool OutputSuspended = false;
    };

    /// @brief Plays rumble clips on pads: layers, scales, loops and stops them, and mixes each pad.
    ///
    /// The application's one writer of every pad's motors, reached as SystemContext::Haptics and
    /// Application::GetHaptics. Once per frame Update advances every instance by the frame's unscaled
    /// time, retires finished non-looping instances and completed fades, stops instances whose world
    /// closed, mixes every pad (MixRumble over its instances, scaled by the master intensity) and
    /// hands each slot's levels to the host. It runs the same headless and on a dedicated host, where
    /// the host simply has no device to write.
    ///
    /// Main-thread only, like the rest of the frame.
    class HapticsEngine
    {
    public:
        /// @brief Scope during which the engine treats every play as a reconciliation replay.
        ///
        /// While one is held, Play starts nothing and returns a null handle, so a Sim system re-run
        /// to re-derive predicted state never re-triggers a clip it already started on the live tick.
        /// Every other call works as usual. Scopes nest.
        class ReplayScope
        {
        public:
            /// @brief Ends the scope.
            ~ReplayScope();

            /// @brief Not copyable: one scope, one end.
            ReplayScope(const ReplayScope&) = delete;

            /// @brief Not assignable.
            ReplayScope& operator=(const ReplayScope&) = delete;

        private:
            friend class HapticsEngine;

            explicit ReplayScope(HapticsEngine& engine);

            /// @brief The engine whose replay depth this scope holds.
            HapticsEngine& m_Engine;
        };

        /// @brief Constructs an engine over its host hooks.
        /// @param info  How it resolves seats and worlds, and where its output goes.
        explicit HapticsEngine(HapticsEngineInfo info = {});

        /// @brief Destroys the engine, releasing every playing clip.
        ~HapticsEngine();

        /// @brief Not copyable: instances are named by slot.
        HapticsEngine(const HapticsEngine&) = delete;

        /// @brief Not copyable: instances are named by slot.
        HapticsEngine& operator=(const HapticsEngine&) = delete;

        /// @brief Starts a clip on a target.
        ///
        /// The instance starts at time zero and first sounds on the next Update. A clip that is not
        /// loaded starts nothing and logs once per clip; so does a play inside a ReplayScope (without
        /// the log) and any play on the inert engine.
        /// @param target  Where the clip plays.
        /// @param clip    The clip; held resident while the instance plays.
        /// @param params  Intensity, loop override and owning world.
        /// @return The instance's handle, or a null handle when nothing started.
        RumbleHandle Play(const RumbleTarget& target, const AssetHandle<RumbleClip>& clip,
                          const RumbleParams& params = {});

        /// @brief Sets a playing instance's intensity; ignored for a stale handle.
        /// @param handle     The instance.
        /// @param intensity  The new intensity, clamped to 0 or more.
        void SetIntensity(RumbleHandle handle, f32 intensity);

        /// @brief Stops a playing instance, at once or over a fade; ignored for a stale handle.
        ///
        /// With a fade the instance keeps playing, scaled down linearly to zero over @p fadeSeconds,
        /// and retires when the fade completes. A second Stop on a fading instance restarts the fade
        /// from its current level only if the new fade ends sooner.
        /// @param handle       The instance.
        /// @param fadeSeconds  The fade's length; 0 or less stops at once.
        void Stop(RumbleHandle handle, f32 fadeSeconds = 0.0f);

        /// @brief Whether a handle names a live instance (a fading one included).
        /// @param handle  The instance.
        [[nodiscard]] bool IsPlaying(RumbleHandle handle) const;

        /// @brief Stops at once every instance playing on a target.
        /// @param target  The target, matched exactly (a seat target does not stop a pad target
        ///                that happens to name the seat's pad).
        void StopAll(const RumbleTarget& target);

        /// @brief Sets the master intensity every pad's mix is scaled by, clamped to [0, 1].
        ///
        /// The hook for a player's vibration setting; 1 by default.
        /// @param intensity  The master intensity.
        void SetMasterIntensity(f32 intensity);

        /// @brief Returns the master intensity.
        [[nodiscard]] f32 GetMasterIntensity() const;

        /// @brief Returns every live instance playing on a target, in slot order.
        /// @param target  The target, matched exactly.
        [[nodiscard]] vector<RumbleInstanceInfo> GetInstances(const RumbleTarget& target) const;

        /// @brief Returns every live instance, in slot order.
        [[nodiscard]] vector<RumbleInstanceInfo> GetAllInstances() const;

        /// @brief Returns a pad's mixed levels as of the last Update, before any output suspension.
        /// @param pad  The pad slot; an out-of-range slot reads zero.
        [[nodiscard]] RumbleChannels GetOutput(GamepadId pad) const;

        /// @brief Whether the last Update silenced the device output.
        [[nodiscard]] bool IsOutputSuspended() const;

        /// @brief Advances, retires, mixes and writes, once per frame.
        /// @param frame  The frame's delta and output suspension.
        void Update(const HapticsFrameInfo& frame);

        /// @brief Opens a scope during which every play is treated as a replay (see ReplayScope).
        [[nodiscard]] ReplayScope BeginReplay();

        /// @brief Whether a ReplayScope is held.
        [[nodiscard]] bool IsReplaying() const;

        /// @brief Whether this is the inert engine, which starts nothing.
        [[nodiscard]] bool IsInert() const;

    private:
        /// @brief Tags the private constructor of the inert engine.
        struct InertTag
        {
        };

        friend HapticsEngine& GetInertEngine();

        /// @brief Constructs the inert engine.
        explicit HapticsEngine(InertTag);

        /// @brief The instance table, the per-pad mix and the logged-clip set.
        struct State;

        /// @brief The instance table, the per-pad mix and the logged-clip set.
        Unique<State> m_State;
    };

    /// @brief Returns the process's inert engine: every play starts nothing.
    ///
    /// What a SystemContext a caller assembles without an application binds its Haptics to, so a
    /// test or tool context compiles without naming one and a system playing rumble through it does
    /// nothing. The Application points its contexts at its live engine instead.
    [[nodiscard]] HapticsEngine& GetInertEngine();
}
