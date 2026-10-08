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

        /// @brief One clip playing once through on a pad, owned by a scope.
        struct OneShot
        {
            /// @brief The owning scope.
            PresentationScopeId Scope;
            /// @brief The pad it plays on.
            GamepadId Pad = GamepadId::None;
            /// @brief The clip, held resident while it plays.
            AssetHandle<RumbleClip> Clip;
            /// @brief Seconds it has advanced.
            f32 Time = 0.0f;
            /// @brief The intensity it plays at.
            f32 Intensity = 1.0f;
            /// @brief Not yet past an update its scope was not Held at, so it next sounds time zero.
            bool Fresh = true;
        };

        /// @brief One frame's submitted level on a pad, owned by a scope.
        struct Layer
        {
            /// @brief The owning scope.
            PresentationScopeId Scope;
            /// @brief The pad it was submitted to.
            GamepadId Pad = GamepadId::None;
            /// @brief Its levels.
            RumbleChannels Channels;
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
        /// @brief The registry every owning scope is judged in.
        const PresentationScopes* Scopes = nullptr;
        /// @brief Where the mix goes.
        HapticsEngineInfo Info;
        /// @brief The one-shots, in the order they were played.
        vector<OneShot> OneShots;
        /// @brief The layers submitted since the last Update.
        vector<Layer> Pending;
        /// @brief The layers the last Update mixed, kept for inspection.
        vector<Layer> Mixed;
        /// @brief Each pad slot's mix as of the last Update, before suspension.
        std::array<RumbleChannels, Input::MaxGamepads> Output{};
        /// @brief Whether the last Update silenced the output.
        bool Suspended = false;
        /// @brief The master intensity.
        f32 Master = 1.0f;
        /// @brief Clips already reported as not loaded, so each warns once.
        std::unordered_set<u64> LoggedClips;
    };

    HapticsEngine::HapticsEngine(const PresentationScopes& scopes, HapticsEngineInfo info)
        : m_State(CreateUnique<State>())
    {
        m_State->Scopes = &scopes;
        m_State->Info = std::move(info);
    }

    HapticsEngine::~HapticsEngine() = default;

    void HapticsEngine::PlayOneShot(const PresentationScopeId scope, const GamepadId pad,
                                    const AssetHandle<RumbleClip>& clip, const f32 intensity)
    {
        State& state = *m_State;
        if (!clip.IsLoaded())
        {
            if (state.LoggedClips.insert(clip.Id().Value).second)
            {
                Log::Warn("Haptics: rumble clip 0x{:016X} is not loaded; nothing plays",
                          clip.Id().Value);
            }
            return;
        }
        if (!PadIndex(pad) || state.Scopes->GetState(scope) == PresentationState::Closed)
        {
            return;
        }
        state.OneShots.push_back(OneShot{
            .Scope = scope, .Pad = pad, .Clip = clip, .Intensity = std::max(0.0f, intensity)});
    }

    void HapticsEngine::SubmitLayer(const PresentationScopeId scope, const GamepadId pad,
                                    const RumbleChannels& channels)
    {
        if (PadIndex(pad))
        {
            m_State->Pending.push_back(Layer{.Scope = scope, .Pad = pad, .Channels = channels});
        }
    }

    void HapticsEngine::StopOneShots(const PresentationScopeId scope, const GamepadId pad)
    {
        std::erase_if(m_State->OneShots, [&](const OneShot& oneShot)
                      { return oneShot.Scope == scope && oneShot.Pad == pad; });
    }

    void HapticsEngine::SetMasterIntensity(const f32 intensity)
    {
        m_State->Master = std::isfinite(intensity) ? Clamp01(intensity) : 1.0f;
    }

    f32 HapticsEngine::GetMasterIntensity() const
    {
        return m_State->Master;
    }

    vector<RumbleOneShotInfo> HapticsEngine::GetOneShots() const
    {
        vector<RumbleOneShotInfo> out;
        out.reserve(m_State->OneShots.size());
        for (const OneShot& oneShot : m_State->OneShots)
        {
            const RumbleClip* clip = oneShot.Clip.IsLoaded() ? oneShot.Clip.Get() : nullptr;
            out.push_back(RumbleOneShotInfo{
                .Scope = oneShot.Scope,
                .State = m_State->Scopes->GetState(oneShot.Scope),
                .Gamepad = oneShot.Pad,
                .Clip = oneShot.Clip.Id(),
                .Time = oneShot.Time,
                .Duration = clip != nullptr ? clip->GetDuration() : 0.0f,
                .Intensity = oneShot.Intensity,
            });
        }
        return out;
    }

    vector<RumbleLayerInfo> HapticsEngine::GetLayers() const
    {
        vector<RumbleLayerInfo> out;
        out.reserve(m_State->Mixed.size());
        for (const Layer& layer : m_State->Mixed)
        {
            out.push_back(RumbleLayerInfo{.Scope = layer.Scope,
                                          .State = m_State->Scopes->GetState(layer.Scope),
                                          .Gamepad = layer.Pad,
                                          .Channels = layer.Channels});
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

    const PresentationScopes& HapticsEngine::GetScopes() const
    {
        return *m_State->Scopes;
    }

    void HapticsEngine::Update(const HapticsFrameInfo& frame)
    {
        State& state = *m_State;
        const f32 delta = std::max(0.0f, frame.Delta);
        std::swap(state.Mixed, state.Pending);
        state.Pending.clear();

        // Advance by scope: Closed drops, Held freezes (a fresh one stays fresh), Muted and Live
        // advance. A fresh one holds at zero on its first advancing update, so its start sounds first.
        std::erase_if(state.OneShots,
                      [&](OneShot& oneShot)
                      {
                          const PresentationState scope = state.Scopes->GetState(oneShot.Scope);
                          if (scope == PresentationState::Closed || !oneShot.Clip.IsLoaded())
                          {
                              return true;
                          }
                          if (scope == PresentationState::Held)
                          {
                              return false;
                          }
                          if (oneShot.Fresh)
                          {
                              oneShot.Fresh = false;
                              return false;
                          }
                          oneShot.Time += delta;
                          return oneShot.Time >= oneShot.Clip.Get()->GetDuration();
                      });

        // Mix: each pad takes the maximum over what its Live scopes own.
        std::array<RumbleChannels, Input::MaxGamepads> peaks{};
        for (const OneShot& oneShot : state.OneShots)
        {
            if (state.Scopes->GetState(oneShot.Scope) != PresentationState::Live)
            {
                continue;
            }
            const usize pad = *PadIndex(oneShot.Pad);
            const RumbleChannels level =
                EvaluateClip(oneShot.Clip.Get()->GetData(), oneShot.Time, false);
            peaks[pad] = Max(peaks[pad], ScaleRumble(level, oneShot.Intensity));
        }
        for (const Layer& layer : state.Mixed)
        {
            if (state.Scopes->GetState(layer.Scope) != PresentationState::Live)
            {
                continue;
            }
            const usize pad = *PadIndex(layer.Pad);
            peaks[pad] = Max(peaks[pad], layer.Channels);
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
}
