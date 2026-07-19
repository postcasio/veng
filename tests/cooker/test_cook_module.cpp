// A minimal cook module: exports the cook ABI version + VengCookModuleRegister, and registers one
// importer for the asset type veng_test_module registers the identity of. It links
// veng::cook_interface — the importer contract as headers only — proving a cook module needs
// nothing from the static libveng_cook.
//
// Built twice. VENG_TEST_PROBE_REVERSED emits the same value most-significant byte first, giving a
// second image whose importer produces different bytes from identical sources — the stand-in for a
// rebuilt importer that the cook-cache keying test swaps in.

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
#ifdef VENG_TEST_PROBE_REVERSED
            const Veng::u32 shifts[] = {24U, 16U, 8U, 0U};
#else
            const Veng::u32 shifts[] = {0U, 8U, 16U, 24U};
#endif
            Veng::vector<Veng::u8> blob;
            blob.reserve(4);
            for (const Veng::u32 shift : shifts)
            {
                blob.push_back(static_cast<Veng::u8>((value >> shift) & 0xFFU));
            }
            return blob;
        }
    };
}

extern "C" void VengCookModuleRegister(Veng::Cook::VengCookModuleHost* host)
{
    host->Importers.Register(Veng::CreateUnique<ProbeImporter>());
}

VE_EXPORT_COOK_MODULE_ABI()
