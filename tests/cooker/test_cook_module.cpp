// A minimal cook module: exports the cook ABI version + VengCookModuleRegister, and registers one
// importer for the asset type veng_test_module registers the identity of. It links
// veng::cook_interface — the importer contract as headers only — proving a cook module needs
// nothing from the static libveng_cook.

#include <Veng/Cook/CookModule.h>

#include "module/probe_component.h"

namespace
{
    // Cooks the entry's "value" integer into a four-byte blob, so a round-trip through the archive
    // is checkable without any real asset format.
    class ProbeImporter final : public Veng::Cook::AssetImporter
    {
    public:
        [[nodiscard]] Veng::AssetTypeId Type() const override { return ProbeAssetType; }

        [[nodiscard]] Veng::Result<Veng::vector<Veng::u8>>
        Cook(const Veng::Cook::CookContext&, const Veng::Cook::json& entry) const override
        {
            if (!entry.contains("value") || !entry["value"].is_number_unsigned())
            {
                return std::unexpected("probe asset entry needs an unsigned \"value\"");
            }

            const Veng::u32 value = entry["value"].get<Veng::u32>();
            return Veng::vector<Veng::u8>{static_cast<Veng::u8>(value & 0xFFU),
                                          static_cast<Veng::u8>((value >> 8) & 0xFFU),
                                          static_cast<Veng::u8>((value >> 16) & 0xFFU),
                                          static_cast<Veng::u8>((value >> 24) & 0xFFU)};
        }
    };
}

extern "C" void VengCookModuleRegister(Veng::Cook::VengCookModuleHost* host)
{
    host->Importers.Register(Veng::CreateUnique<ProbeImporter>());
}

VE_EXPORT_COOK_MODULE_ABI()
