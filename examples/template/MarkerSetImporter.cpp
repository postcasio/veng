// The template's cook module: the offline half of its game-defined asset type.
//
// Built as libtemplate_cook by veng_add_game(... COOK_SOURCES ...) and loaded by vengc and by the
// editor beside libtemplate; nothing at runtime loads it. It links veng::cook_interface — the
// importer contract as headers only — rather than the static libveng_cook, so no second copy of
// the cooker's machinery or its process-wide state rides into the dlopened image.
//
// It registers importers and nothing else: the asset type's identity and name register exactly
// once per process, through libtemplate's own VengModuleRegister, which every host reaches.

#include <Veng/Cook/CookModule.h>

#include <fmt/format.h>

#include <cstring>
#include <fstream>

#include "MarkerSet.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // Cooks a `*.markers.json` source into the fixed-stride record + name-heap layout MarkerSet.h
    // declares. Every typed JSON access is guarded first: the cooker builds with JSON_NOEXCEPTION,
    // so an unchecked access would abort rather than report a located error.
    class MarkerSetImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetTypeId Type() const override { return Template::MarkerSetAssetType; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override
        {
            if (!entry.contains("source") || !entry["source"].is_string())
            {
                return std::unexpected("marker set entry needs a string \"source\"");
            }

            const path sourcePath = context.PackDir / entry["source"].get<string>();
            // The source is a binary payload the manifest names only indirectly, so the importer
            // records it: editing the markers must re-cook the pack.
            context.RecordDependency(sourcePath);

            std::ifstream in(sourcePath);
            if (!in)
            {
                return std::unexpected(
                    fmt::format("cannot open marker source '{}'", sourcePath.string()));
            }

            const json source = json::parse(in, nullptr, false);
            if (source.is_discarded() || !source.is_object())
            {
                return std::unexpected(
                    fmt::format("marker source '{}' is not a JSON object", sourcePath.string()));
            }
            if (!source.contains("markers") || !source["markers"].is_array())
            {
                return std::unexpected(fmt::format("marker source '{}' needs a \"markers\" array",
                                                   sourcePath.string()));
            }

            vector<Template::CookedMarkerRecord> records;
            vector<char> nameHeap;
            for (const json& marker : source["markers"])
            {
                if (!marker.is_object() || !marker.contains("name") || !marker["name"].is_string())
                {
                    return std::unexpected(
                        fmt::format("marker source '{}': every marker needs a string \"name\"",
                                    sourcePath.string()));
                }
                if (!marker.contains("position") || !marker["position"].is_array() ||
                    marker["position"].size() != 3)
                {
                    return std::unexpected(
                        fmt::format("marker source '{}': marker '{}' needs a 3-element "
                                    "\"position\"",
                                    sourcePath.string(), marker["name"].get<string>()));
                }

                const string name = marker["name"].get<string>();
                Template::CookedMarkerRecord record{
                    .Position = {},
                    .NameOffset = static_cast<u32>(nameHeap.size()),
                    .NameLength = static_cast<u32>(name.size()),
                };
                for (usize axis = 0; axis < 3; ++axis)
                {
                    const json& component = marker["position"][axis];
                    if (!component.is_number())
                    {
                        return std::unexpected(
                            fmt::format("marker source '{}': marker '{}' has a non-numeric "
                                        "position component",
                                        sourcePath.string(), name));
                    }
                    record.Position[axis] = component.get<f32>();
                }

                nameHeap.insert(nameHeap.end(), name.begin(), name.end());
                records.push_back(record);
            }

            const Template::CookedMarkerSetHeader header{
                .Magic = Template::MarkerSetMagic,
                .Version = Template::MarkerSetVersion,
                .MarkerCount = static_cast<u32>(records.size()),
                .NameHeapBytes = static_cast<u32>(nameHeap.size()),
            };

            const usize recordBytes = records.size() * sizeof(Template::CookedMarkerRecord);
            vector<u8> blob(sizeof(header) + recordBytes + nameHeap.size());
            std::memcpy(blob.data(), &header, sizeof(header));
            if (recordBytes > 0)
            {
                std::memcpy(blob.data() + sizeof(header), records.data(), recordBytes);
            }
            if (!nameHeap.empty())
            {
                std::memcpy(blob.data() + sizeof(header) + recordBytes, nameHeap.data(),
                            nameHeap.size());
            }

            return blob;
        }
    };
}

extern "C" void VengCookModuleRegister(VengCookModuleHost* host)
{
    // Qualified: Veng::Cook re-exports CreateUnique, so the unqualified name is ambiguous under
    // both using-directives above.
    host->Importers.Register(Veng::CreateUnique<MarkerSetImporter>());
}

VE_EXPORT_COOK_MODULE_ABI()
