#include "AudioBusGraphLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob>
    AudioBusGraphLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                              TaskSystem& /*tasks*/, TypeRegistry& types, AssetId id,
                              std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedAudioBusGraphHeader))
        {
            return std::unexpected(
                Corrupt(id, "audio bus graph: cooked blob smaller than CookedAudioBusGraphHeader"));
        }

        CookedAudioBusGraphHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedAudioBusGraphVersion)
        {
            return std::unexpected(Corrupt(
                id,
                fmt::format("audio bus graph: blob version {} does not match expected version {}",
                            header.Version, CookedAudioBusGraphVersion)));
        }

        const usize cursor = sizeof(CookedAudioBusGraphHeader);
        if (cooked.size() < cursor + header.RecordBytes)
        {
            return std::unexpected(Corrupt(id, "audio bus graph: cooked blob truncated"));
        }

        const std::span<const u8> record = cooked.subspan(cursor, header.RecordBytes);

        Audio::AudioBusGraphData data;
        const VoidResult read =
            ReadFields(record, &data, types.Info(TypeIdOf<Audio::AudioBusGraphData>()), types);
        if (!read)
        {
            return std::unexpected(Corrupt(id, read.error()));
        }

        const Ref<Audio::AudioBusGraph> graph = Audio::AudioBusGraph::Create(std::move(data));
        return Detail::LoadJob{.Resource = Detail::RefAny(graph)};
    }
}
