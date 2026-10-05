#include "RumbleClipImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

namespace Veng::Cook
{
    namespace
    {
        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }
    }

    Result<vector<u8>> RumbleClipImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("rumble clip importer: missing or invalid 'source'");
        }
        const string id = entry.contains("id") && entry["id"].is_string()
                              ? entry["id"].get<string>()
                              : string("<unknown id>");

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const Result<json> doc = ReadJsonFile(sourcePath, "rumble clip importer");
        if (!doc)
        {
            return std::unexpected(doc.error());
        }

        // A clip references only engine builtins, so it needs no game module.
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        const TypeInfo& info = registry.Info(TypeIdOf<Haptics::RumbleClipData>());

        Haptics::RumbleClipData clip;
        if (const VoidResult bound = JsonReadFields(&clip, info, *doc, registry); !bound)
        {
            return std::unexpected(fmt::format("rumble clip importer: asset {} ('{}'): {}", id,
                                               sourcePath.string(), bound.error()));
        }
        if (const string bad = Haptics::CheckRumbleClip(clip); !bad.empty())
        {
            return std::unexpected(fmt::format("rumble clip importer: asset {} ('{}'): {}", id,
                                               sourcePath.string(), bad));
        }

        vector<u8> record;
        WriteFields(record, &clip, info, registry);

        const CookedRumbleClipHeader header{.Version = CookedRumbleClipVersion,
                                            .RecordBytes = static_cast<u32>(record.size())};
        vector<u8> blob;
        blob.reserve(sizeof(header) + record.size());
        Append(blob, header);
        blob.insert(blob.end(), record.begin(), record.end());
        return blob;
    }

    void RegisterRumbleClipImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<RumbleClipImporter>());
    }
}
