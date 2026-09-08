#include <Veng/Audio/AudioBusGraph.h>

#include <fmt/format.h>

#include <unordered_map>
#include <unordered_set>

namespace Veng::Audio
{
    AudioBusGraphData DefaultAudioBusGraphData()
    {
        AudioBusGraphData data;
        data.Buses.push_back(AudioBusDef{.Id = string(AudioBuses::MasterName), .Parent = ""});
        data.Buses.push_back(AudioBusDef{.Id = string(AudioBuses::MusicName),
                                         .Parent = string(AudioBuses::MasterName)});
        data.Buses.push_back(AudioBusDef{.Id = string(AudioBuses::SFXName),
                                         .Parent = string(AudioBuses::MasterName)});
        data.Buses.push_back(AudioBusDef{.Id = string(AudioBuses::UIName),
                                         .Parent = string(AudioBuses::MasterName)});
        data.Buses.push_back(AudioBusDef{.Id = string(AudioBuses::AmbienceName),
                                         .Parent = string(AudioBuses::MasterName)});
        return data;
    }

    VoidResult ValidateAudioBusGraph(const AudioBusGraphData& data)
    {
        if (data.Buses.empty())
        {
            return std::unexpected(string("bus graph declares no buses; it must declare the "
                                          "single root 'Master'"));
        }
        if (data.Buses.size() > MaxBuses)
        {
            return std::unexpected(
                fmt::format("bus graph declares {} buses, exceeding the cap of {}",
                            data.Buses.size(), MaxBuses));
        }

        // Unique, non-empty names, and no interned-hash collision between two distinct names.
        std::unordered_map<string, const AudioBusDef*> byName;
        std::unordered_map<u64, string> byHash;
        for (const AudioBusDef& bus : data.Buses)
        {
            if (bus.Id.empty())
            {
                return std::unexpected(string("a bus has an empty 'Id'"));
            }
            if (!byName.emplace(bus.Id, &bus).second)
            {
                return std::unexpected(
                    fmt::format("bus id '{}' is declared more than once", bus.Id));
            }
            const u64 hash = BusId{bus.Id}.Value;
            const auto [it, inserted] = byHash.emplace(hash, bus.Id);
            if (!inserted)
            {
                return std::unexpected(fmt::format(
                    "bus ids '{}' and '{}' collide under the interned hash", it->second, bus.Id));
            }
        }

        // Exactly one root (empty Parent), and it is Master; every other Parent resolves.
        const AudioBusDef* root = nullptr;
        for (const AudioBusDef& bus : data.Buses)
        {
            if (bus.Parent.empty())
            {
                if (root != nullptr)
                {
                    return std::unexpected(fmt::format("bus graph has more than one root ('{}' and "
                                                       "'{}'); exactly one bus may have "
                                                       "no parent",
                                                       root->Id, bus.Id));
                }
                root = &bus;
                continue;
            }
            if (!byName.contains(bus.Parent))
            {
                return std::unexpected(fmt::format(
                    "bus '{}' names parent '{}', which no bus declares", bus.Id, bus.Parent));
            }
        }
        if (root == nullptr)
        {
            return std::unexpected(
                string("bus graph has no root; exactly one bus must have no parent"));
        }
        if (root->Id != AudioBuses::MasterName)
        {
            return std::unexpected(fmt::format("the root bus must be named '{}', not '{}'",
                                               AudioBuses::MasterName, root->Id));
        }

        // Acyclic, and depth within the cap: every bus's parent chain reaches the root without
        // exceeding MaxBusDepth steps (a longer walk means either a cycle or an over-deep tree).
        for (const AudioBusDef& bus : data.Buses)
        {
            const AudioBusDef* cursor = &bus;
            u32 depth = 0;
            while (!cursor->Parent.empty())
            {
                ++depth;
                if (depth > MaxBusDepth)
                {
                    return std::unexpected(fmt::format(
                        "bus '{}' exceeds the maximum depth of {} (or its parent chain cycles)",
                        bus.Id, MaxBusDepth));
                }
                cursor = byName.at(cursor->Parent);
            }
        }

        return {};
    }

    AudioBusGraph::AudioBusGraph(AudioBusGraphData data) : m_Data(std::move(data)) {}

    Ref<AudioBusGraph> AudioBusGraph::Create(AudioBusGraphData data)
    {
        return Ref<AudioBusGraph>(new AudioBusGraph(std::move(data)));
    }
}
