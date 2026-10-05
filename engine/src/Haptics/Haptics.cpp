#include <Veng/Haptics/Haptics.h>

#include <Veng/Log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace Veng::Haptics
{
    namespace
    {
        f32 Clamp01(const f32 value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        RumbleChannels Max(const RumbleChannels& a, const RumbleChannels& b)
        {
            return RumbleChannels{.LowFrequency = std::max(a.LowFrequency, b.LowFrequency),
                                  .HighFrequency = std::max(a.HighFrequency, b.HighFrequency),
                                  .LeftTrigger = std::max(a.LeftTrigger, b.LeftTrigger),
                                  .RightTrigger = std::max(a.RightTrigger, b.RightTrigger)};
        }

        // The slot index a pad id names, or nullopt for None and anything past the slot table.
        optional<usize> PadIndex(const GamepadId pad)
        {
            const auto index = static_cast<usize>(pad);
            if (pad == GamepadId::None || index >= Input::MaxGamepads)
            {
                return std::nullopt;
            }
            return index;
        }

        /// @brief One slot of the instance table.
        struct Instance
        {
            /// @brief Bumped each time the slot is taken; a handle naming an older value is stale.
            u32 Generation = 0;
            /// @brief Whether the slot holds a playing instance.
            bool Live = false;
            /// @brief What the instance plays on.
            RumbleTarget Target;
            /// @brief The clip, held resident while the instance plays.
            AssetHandle<RumbleClip> Clip;
            /// @brief Seconds since the instance started.
            f32 Time = 0.0f;
            /// @brief The instance's intensity.
            f32 Intensity = 1.0f;
            /// @brief The effective loop setting.
            bool Loop = false;
            /// @brief The owning world; invalid for an application-owned instance.
            WorldInstanceId World;
            /// @brief Whether a fading Stop is ramping the instance out.
            bool Stopping = false;
            /// @brief The fade's length the remaining time is a fraction of.
            f32 FadeTotal = 0.0f;
            /// @brief Seconds of fade left.
            f32 FadeRemaining = 0.0f;
            /// @brief Started since the last Update, so that Update plays it from time zero.
            bool Fresh = true;
            /// @brief Whether the owning world was paused at the last Update.
            bool Paused = false;
            /// @brief The pad the target resolved to at the last Update.
            GamepadId Pad = GamepadId::None;

            /// @brief The stop fade's remaining fraction, 1 when not stopping.
            [[nodiscard]] f32 FadeFactor() const
            {
                return Stopping && FadeTotal > 0.0f ? std::max(0.0f, FadeRemaining / FadeTotal)
                                                    : 1.0f;
            }
        };
    }

    RumbleChannels EvaluateClip(const RumbleClipData& clip, f32 time, const bool loop)
    {
        if (!(clip.Duration > 0.0f) || time < 0.0f)
        {
            return {};
        }
        if (loop)
        {
            time = std::fmod(time, clip.Duration);
        }
        else if (time >= clip.Duration)
        {
            return {};
        }
        return RumbleChannels{.LowFrequency = Clamp01(clip.LowFrequency.Evaluate(time)),
                              .HighFrequency = Clamp01(clip.HighFrequency.Evaluate(time)),
                              .LeftTrigger = Clamp01(clip.LeftTrigger.Evaluate(time)),
                              .RightTrigger = Clamp01(clip.RightTrigger.Evaluate(time))};
    }

    RumbleChannels ScaleRumble(const RumbleChannels& channels, const f32 scale)
    {
        return RumbleChannels{.LowFrequency = channels.LowFrequency * scale,
                              .HighFrequency = channels.HighFrequency * scale,
                              .LeftTrigger = channels.LeftTrigger * scale,
                              .RightTrigger = channels.RightTrigger * scale};
    }

    RumbleChannels MixRumble(const std::span<const RumbleChannels> layers, const f32 master)
    {
        RumbleChannels mixed;
        for (const RumbleChannels& layer : layers)
        {
            mixed = Max(mixed, layer);
        }
        const RumbleChannels scaled = ScaleRumble(mixed, master);
        return RumbleChannels{.LowFrequency = Clamp01(scaled.LowFrequency),
                              .HighFrequency = Clamp01(scaled.HighFrequency),
                              .LeftTrigger = Clamp01(scaled.LeftTrigger),
                              .RightTrigger = Clamp01(scaled.RightTrigger)};
    }

    struct HapticsEngine::State
    {
        /// @brief The host hooks.
        HapticsEngineInfo Info;
        /// @brief Whether this is the inert engine.
        bool Inert = false;
        /// @brief The instance table; a retired slot is reused with its generation bumped.
        vector<Instance> Instances;
        /// @brief Each pad slot's mix as of the last Update, before suspension.
        std::array<RumbleChannels, Input::MaxGamepads> Output{};
        /// @brief Whether the last Update silenced the output.
        bool Suspended = false;
        /// @brief The master intensity.
        f32 Master = 1.0f;
        /// @brief How many ReplayScopes are held.
        u32 ReplayDepth = 0;
        /// @brief Clips already reported as not loaded, so each warns once.
        std::unordered_set<u64> LoggedClips;

        /// @brief Returns the live instance a handle names, or nullptr for a stale one.
        Instance* Find(const RumbleHandle handle)
        {
            if (!handle.IsValid() || handle.Slot >= Instances.size())
            {
                return nullptr;
            }
            Instance& instance = Instances[handle.Slot];
            return instance.Live && instance.Generation == handle.Generation ? &instance : nullptr;
        }

        /// @brief Empties a slot, releasing its clip.
        static void Retire(Instance& instance)
        {
            instance.Live = false;
            instance.Clip = {};
        }

        /// @brief Resolves a target to the pad it plays on this frame.
        [[nodiscard]] GamepadId Resolve(const RumbleTarget& target) const
        {
            switch (target.Kind)
            {
            case RumbleTargetKind::Seat:
                return Info.ResolveSeat ? Info.ResolveSeat(target.Seat) : GamepadId::None;
            case RumbleTargetKind::Gamepad:
                return target.Gamepad;
            case RumbleTargetKind::None:
                break;
            }
            return GamepadId::None;
        }

        /// @brief Builds the inspection view of a live instance.
        [[nodiscard]] static RumbleInstanceInfo Describe(const Instance& instance, const usize slot)
        {
            const RumbleClip* clip = instance.Clip.IsLoaded() ? instance.Clip.Get() : nullptr;
            return RumbleInstanceInfo{
                .Handle = {.Slot = static_cast<u32>(slot), .Generation = instance.Generation},
                .Target = instance.Target,
                .Gamepad = instance.Pad,
                .Clip = instance.Clip.Id(),
                .Time = instance.Time,
                .Duration = clip != nullptr ? clip->GetDuration() : 0.0f,
                .Intensity = instance.Intensity,
                .Fade = instance.FadeFactor(),
                .Loop = instance.Loop,
                .Stopping = instance.Stopping,
                .Paused = instance.Paused,
                .World = instance.World,
            };
        }
    };

    HapticsEngine::ReplayScope::ReplayScope(HapticsEngine& engine) : m_Engine(engine)
    {
        ++m_Engine.m_State->ReplayDepth;
    }

    HapticsEngine::ReplayScope::~ReplayScope()
    {
        --m_Engine.m_State->ReplayDepth;
    }

    HapticsEngine::HapticsEngine(HapticsEngineInfo info) : m_State(CreateUnique<State>())
    {
        m_State->Info = std::move(info);
    }

    HapticsEngine::HapticsEngine(InertTag) : m_State(CreateUnique<State>())
    {
        m_State->Inert = true;
    }

    HapticsEngine::~HapticsEngine() = default;

    RumbleHandle HapticsEngine::Play(const RumbleTarget& target,
                                     const AssetHandle<RumbleClip>& clip,
                                     const RumbleParams& params)
    {
        State& state = *m_State;
        if (state.Inert || state.ReplayDepth > 0)
        {
            return {};
        }
        if (!clip.IsLoaded())
        {
            if (state.LoggedClips.insert(clip.Id().Value).second)
            {
                Log::Warn("Haptics: rumble clip 0x{:016X} is not loaded; nothing plays",
                          clip.Id().Value);
            }
            return {};
        }

        const auto free =
            std::ranges::find_if(state.Instances, [](const Instance& slot) { return !slot.Live; });
        const auto slot = static_cast<usize>(free - state.Instances.begin());
        if (free == state.Instances.end())
        {
            state.Instances.emplace_back();
        }

        Instance& instance = state.Instances[slot];
        const u32 generation = instance.Generation + 1u == 0u ? 1u : instance.Generation + 1u;
        instance = Instance{
            .Generation = generation,
            .Live = true,
            .Target = target,
            .Clip = clip,
            .Intensity = std::max(0.0f, params.Intensity),
            .Loop = params.Loop.value_or(clip.Get()->IsLooping()),
            .World = params.World,
        };
        return RumbleHandle{.Slot = static_cast<u32>(slot), .Generation = generation};
    }

    void HapticsEngine::SetIntensity(const RumbleHandle handle, const f32 intensity)
    {
        if (Instance* instance = m_State->Find(handle))
        {
            instance->Intensity = std::max(0.0f, intensity);
        }
    }

    void HapticsEngine::Stop(const RumbleHandle handle, const f32 fadeSeconds)
    {
        Instance* instance = m_State->Find(handle);
        if (instance == nullptr)
        {
            return;
        }
        if (!(fadeSeconds > 0.0f))
        {
            State::Retire(*instance);
            return;
        }
        if (!instance->Stopping)
        {
            instance->Stopping = true;
            instance->FadeTotal = fadeSeconds;
            instance->FadeRemaining = fadeSeconds;
            return;
        }
        // Already fading: keep the level continuous, and take the new fade only if it ends sooner.
        if (fadeSeconds < instance->FadeRemaining)
        {
            const f32 level = instance->FadeFactor();
            instance->FadeRemaining = fadeSeconds;
            instance->FadeTotal = fadeSeconds / level;
        }
    }

    bool HapticsEngine::IsPlaying(const RumbleHandle handle) const
    {
        return m_State->Find(handle) != nullptr;
    }

    void HapticsEngine::StopAll(const RumbleTarget& target)
    {
        for (Instance& instance : m_State->Instances)
        {
            if (instance.Live && instance.Target == target)
            {
                State::Retire(instance);
            }
        }
    }

    void HapticsEngine::SetMasterIntensity(const f32 intensity)
    {
        m_State->Master = std::isfinite(intensity) ? Clamp01(intensity) : 1.0f;
    }

    f32 HapticsEngine::GetMasterIntensity() const
    {
        return m_State->Master;
    }

    vector<RumbleInstanceInfo> HapticsEngine::GetInstances(const RumbleTarget& target) const
    {
        vector<RumbleInstanceInfo> out;
        for (usize slot = 0; slot < m_State->Instances.size(); ++slot)
        {
            const Instance& instance = m_State->Instances[slot];
            if (instance.Live && instance.Target == target)
            {
                out.push_back(State::Describe(instance, slot));
            }
        }
        return out;
    }

    vector<RumbleInstanceInfo> HapticsEngine::GetAllInstances() const
    {
        vector<RumbleInstanceInfo> out;
        for (usize slot = 0; slot < m_State->Instances.size(); ++slot)
        {
            if (const Instance& instance = m_State->Instances[slot]; instance.Live)
            {
                out.push_back(State::Describe(instance, slot));
            }
        }
        return out;
    }

    RumbleChannels HapticsEngine::GetOutput(const GamepadId pad) const
    {
        const optional<usize> index = PadIndex(pad);
        return index ? m_State->Output[*index] : RumbleChannels{};
    }

    bool HapticsEngine::IsOutputSuspended() const
    {
        return m_State->Suspended;
    }

    void HapticsEngine::Update(const HapticsFrameInfo& frame)
    {
        State& state = *m_State;
        const f32 delta = std::max(0.0f, frame.Delta);

        // Advance and retire. A fresh instance holds at zero so its first mix is the clip's start.
        for (Instance& instance : state.Instances)
        {
            if (!instance.Live)
            {
                continue;
            }

            const HapticsWorldState world = instance.World.IsValid() && state.Info.WorldState
                                                ? state.Info.WorldState(instance.World)
                                                : HapticsWorldState::Open;
            if (world == HapticsWorldState::Closed || !instance.Clip.IsLoaded())
            {
                State::Retire(instance);
                continue;
            }
            instance.Paused = world == HapticsWorldState::Paused;
            if (instance.Paused)
            {
                continue;
            }
            if (instance.Fresh)
            {
                instance.Fresh = false;
                continue;
            }

            instance.Time += delta;
            if (instance.Stopping)
            {
                instance.FadeRemaining -= delta;
                if (instance.FadeRemaining <= 0.0f)
                {
                    State::Retire(instance);
                    continue;
                }
            }
            if (!instance.Loop && instance.Time >= instance.Clip.Get()->GetDuration())
            {
                State::Retire(instance);
            }
        }

        // Mix: each pad takes the maximum over the instances resolving to it.
        std::array<RumbleChannels, Input::MaxGamepads> peaks{};
        for (Instance& instance : state.Instances)
        {
            if (!instance.Live)
            {
                continue;
            }
            instance.Pad = state.Resolve(instance.Target);
            const optional<usize> pad = PadIndex(instance.Pad);
            if (!pad || instance.Paused)
            {
                continue;
            }
            const RumbleChannels level =
                EvaluateClip(instance.Clip.Get()->GetData(), instance.Time, instance.Loop);
            peaks[*pad] =
                Max(peaks[*pad], ScaleRumble(level, instance.Intensity * instance.FadeFactor()));
        }

        state.Suspended = frame.OutputSuspended;
        for (usize pad = 0; pad < Input::MaxGamepads; ++pad)
        {
            state.Output[pad] = MixRumble(std::span(&peaks[pad], 1), state.Master);
            if (state.Info.WriteMotors)
            {
                state.Info.WriteMotors(static_cast<GamepadId>(pad),
                                       state.Suspended ? RumbleChannels{} : state.Output[pad]);
            }
        }
    }

    HapticsEngine::ReplayScope HapticsEngine::BeginReplay()
    {
        return ReplayScope(*this);
    }

    bool HapticsEngine::IsReplaying() const
    {
        return m_State->ReplayDepth > 0;
    }

    bool HapticsEngine::IsInert() const
    {
        return m_State->Inert;
    }

    HapticsEngine& GetInertEngine()
    {
        static HapticsEngine s_Inert{HapticsEngine::InertTag{}};
        return s_Inert;
    }
}
