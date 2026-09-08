#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Result.h>

namespace Veng::Audio
{
    /// @brief One bus of an authored bus graph: its name, its parent, and its default DSP.
    ///
    /// A reflected record cooked from an `.audiobusgraph.json` entry. Parent names the bus this one
    /// mixes into — `Master` for a root child, or a previously-defined bus — so the authored order
    /// lists a parent before its children. The DSP defaults seed the runtime bus; lowpass and
    /// reverb-send take effect only on a leaf bus (a bus with children ignores them).
    struct AudioBusDef
    {
        /// @brief The bus name; interned to a BusId. Unique within the graph, non-empty.
        string Id;
        /// @brief The parent bus name; empty only for the single root (which must be `Master`).
        string Parent;
        /// @brief The bus's initial linear gain; composes down the tree.
        f32 DefaultGain = 1.0f;
        /// @brief The bus's low-pass cutoff in Hz (leaf-only; 0 bypasses).
        f32 LowpassCutoff = 0.0f;
        /// @brief The bus's send into the master reverb, 0..1 (leaf-only).
        f32 ReverbSend = 0.0f;
    };

    /// @brief The reflected on-disk payload of a bus graph: the complete list of buses.
    ///
    /// The single reflected record the cook writes and the loader reads through the shared
    /// WriteFields/ReadFields encoder, so a bus graph needs no bespoke binary format. An authored
    /// graph is the complete topology — exactly one root `Master`, every other bus reachable from
    /// it — and replaces the engine's roots-only default rather than extending it.
    struct AudioBusGraphData
    {
        /// @brief The buses, each naming its parent; a parent is listed before its children.
        vector<AudioBusDef> Buses;
    };

    /// @brief Returns the roots-only default graph: Master with the four generic roots beneath it.
    ///
    /// The topology an unconfigured consumer runs on — `Master ⊃ {Music, SFX, UI, Ambience}` at
    /// unity gain, no DSP — reproducing the engine's historical fixed bus set exactly.
    /// @return The default graph data.
    [[nodiscard]] AudioBusGraphData DefaultAudioBusGraphData();

    /// @brief Validates an authored bus graph against the engine's topology rules.
    ///
    /// Checks: exactly one root and it is `Master`; every non-root Parent resolves to a declared
    /// bus; the graph is acyclic; depth ≤ MaxBusDepth; bus count ≤ MaxBuses; and no two bus names
    /// collide under the interned BusId hash. A failing check returns a descriptive message.
    /// @param data  The authored graph.
    /// @return Success, or a message naming the first rule broken.
    [[nodiscard]] VoidResult ValidateAudioBusGraph(const AudioBusGraphData& data);

    /// @brief Cached, immutable cooked bus-graph asset: a game's complete mixer topology.
    ///
    /// A CPU-only asset with no GPU resource; load it through AssetManager::Load by AssetId like any
    /// other asset, then adopt it into the mixer with AudioEngine::ConfigureBusGraph. The asset
    /// carries the authored data verbatim; validation and flattening happen at adoption.
    class AudioBusGraph
    {
    public:
        /// @brief Creates a graph asset from its decoded record.
        /// @param data  The decoded bus list.
        /// @return The constructed graph.
        static Ref<AudioBusGraph> Create(AudioBusGraphData data);

        /// @brief Returns the authored graph data.
        [[nodiscard]] const AudioBusGraphData& GetData() const { return m_Data; }

    private:
        explicit AudioBusGraph(AudioBusGraphData data);

        /// @brief The decoded graph record.
        AudioBusGraphData m_Data;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping AudioBusGraph to AssetTypes::AudioBusGraph.
    template <>
    struct AssetTypeTrait<Audio::AudioBusGraph>
    {
        /// @brief The asset type tag for AudioBusGraph.
        static constexpr AssetTypeId Type = AssetTypes::AudioBusGraph;
    };
}

VE_REFLECT(::Veng::Audio::AudioBusDef, 0x96B3AA7CC5B774F2ULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Parent, .DisplayName = "Parent")
VE_FIELD(DefaultGain, .DisplayName = "Default Gain", .Display = {.Min = 0.0})
VE_FIELD(LowpassCutoff, .DisplayName = "Lowpass Cutoff", .Display = {.Min = 0.0})
VE_FIELD(ReverbSend, .DisplayName = "Reverb Send", .Display = {.Min = 0.0, .Max = 1.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::Audio::AudioBusGraphData, 0xC2CEB8B7411CD74BULL)
VE_ARRAY_FIELD(Buses, .DisplayName = "Buses")
VE_REFLECT_END();
