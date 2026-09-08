#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBusGraph.h>
#include <Veng/Settings/SettingsStore.h>

namespace Veng
{
    /// @brief The input to an audio resolve: the player's chosen values plus the active bus graph.
    ///
    /// Handed to Application::OnResolveAudio so the game can map the player's audio choices onto bus
    /// gains. It carries the persisted per-machine store (the schema and the chosen option/scalar
    /// per setting, read through SettingsStore) and the active mixer bus graph, so a resolver can
    /// name the buses it sets. The engine does not fix what a choice means — that is the game's, in
    /// its resolver; the engine only invokes it, having pre-filled the output with the graph's
    /// default gains, and applies the result.
    struct AudioResolveInput
    {
        /// @brief The per-machine audio store: the schema and the chosen values (borrowed).
        const SettingsStore<SettingsChoices>& Settings;
        /// @brief The active mixer bus graph, so a resolver can name the buses it sets (borrowed).
        const Audio::AudioBusGraphData& BusGraph;
    };

    /// @brief One resolved bus gain: the bus and the linear gain to set on it.
    struct AudioBusGain
    {
        /// @brief The bus the gain applies to.
        Audio::BusId Bus;
        /// @brief The linear gain to set (0 = silent, 1 = unity).
        f32 Gain = 1.0f;
    };

    /// @brief The bus gains an audio resolve produces, applied once per apply via SetBusGain.
    ///
    /// The engine pre-fills this with one entry per active-graph bus at its graph-default gain
    /// before invoking Application::OnResolveAudio, so the identity default resolver leaves every
    /// bus at its authored default. A game's resolver overrides the entries it cares about through
    /// SetGain (which updates an existing entry by bus id, else appends); the engine then applies
    /// each entry through AudioEngine::SetBusGain.
    struct AudioResolveOutput
    {
        /// @brief The gains to set, pre-filled with the active graph's default gains.
        vector<AudioBusGain> Gains;

        /// @brief Sets a bus's gain, updating an existing entry or appending a new one.
        /// @param bus   The bus to set.
        /// @param gain  The linear gain (0 = silent, 1 = unity).
        void SetGain(Audio::BusId bus, f32 gain)
        {
            for (AudioBusGain& entry : Gains)
            {
                if (entry.Bus == bus)
                {
                    entry.Gain = gain;
                    return;
                }
            }
            Gains.push_back(AudioBusGain{.Bus = bus, .Gain = gain});
        }
    };
}
