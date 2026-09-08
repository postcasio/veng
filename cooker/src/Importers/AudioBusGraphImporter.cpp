#include "AudioBusGraphImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioBusGraph.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

namespace Veng::Cook
{
    namespace
    {
        string Located(const string& file, const string& reason)
        {
            return fmt::format("audio bus graph importer: '{}': {}", file, reason);
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }
    }

    Result<vector<u8>> AudioBusGraphImporter::Cook(const CookContext& context,
                                                   const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("audio bus graph importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string file = sourcePath.string();

        const Result<json> docResult = ReadJsonFile(sourcePath, "audio bus graph importer");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }
        const json& doc = *docResult;

        if (!doc.is_object())
        {
            return std::unexpected(Located(file, "the graph source must be a JSON object"));
        }

        // The graph references only engine builtins (its own reflected structs), so it needs no
        // game module: a builtin-only registry supplies the WriteFields encoder.
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);

        const TypeInfo& type = registry.Info(TypeIdOf<Audio::AudioBusGraphData>());
        Audio::AudioBusGraphData graph;
        const VoidResult bound = JsonReadFields(&graph, type, doc, registry);
        if (!bound)
        {
            return std::unexpected(Located(file, bound.error()));
        }

        const VoidResult valid = Audio::ValidateAudioBusGraph(graph);
        if (!valid)
        {
            return std::unexpected(Located(file, valid.error()));
        }

        vector<u8> record;
        WriteFields(record, &graph, type, registry);

        CookedAudioBusGraphHeader header{};
        header.Version = CookedAudioBusGraphVersion;
        header.RecordBytes = static_cast<u32>(record.size());

        vector<u8> blob;
        blob.reserve(sizeof(CookedAudioBusGraphHeader) + record.size());
        Append(blob, header);
        blob.insert(blob.end(), record.begin(), record.end());

        return blob;
    }

    void RegisterAudioBusGraphImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<AudioBusGraphImporter>());
    }
}
